using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace CppDeskRelay;

internal static class Program
{
    private const uint ProtocolMagic = 0x4344534B; // "CDSK"
    private const int DefaultRelayPort = 50999;
    private const int MaxPayloadSize = 32 * 1024 * 1024;
    private const int MaxControlFrame = 64 * 1024;

    private const byte PacketPing              = 0x07;
    private const byte PacketPong              = 0x08;
    private const byte PacketRelayRegister     = 0x50;
    private const byte PacketRelayRegisterAck  = 0x51;
    private const byte PacketRelayLookup       = 0x52;
    private const byte PacketRelayLookupResp   = 0x53;
    private const byte PacketRelayConnectReq   = 0x54;
    private const byte PacketRelayIncomingReq  = 0x55;
    private const byte PacketRelayBridgeAccept = 0x56;
    private const byte PacketRelayBridgeReady  = 0x57;
    private const byte PacketRelayRegisterChallenge = 0x5A;
    private const byte PacketRelayRegisterProof     = 0x5B;

    private const byte OwnerCap = 1;        // trailing byte on RELAY_REGISTER: "I can prove ownership"
    private const byte ModeProve = 0;       // challenge: answer with HMAC(key, label || deskId || nonce)
    private const byte ModeEnroll = 1;      // challenge: answer with the key itself
    private const int OwnerNonceLength = 16;
    private const int OwnerKeyLength = 32;
    private const int OwnerMaxRecords = 100_000;
    private static readonly TimeSpan OwnerTtl = TimeSpan.FromDays(7);   // an ID nobody has used for this long is free again
    private static readonly byte[] OwnerLabel = Encoding.ASCII.GetBytes("CppDesk-relay-owner-v1");

    // Desk IDs whose owner key this relay holds. A host's first registration enrolls its key (trust on first
    // use); later ones must answer a fresh challenge, so knowing an ID is not enough to take it over.
    // shortcut: in memory only, so a relay restart forgets every owner and the first registrant wins again; persist when relays run for months.
    private sealed record OwnerRecord(byte[] Key, DateTime LastSeenUtc);
    private static readonly Dictionary<ulong, OwnerRecord> Owners = new();

    private sealed record RegisteredHost(
        ulong DeskId,
        string Hostname,
        string Ip,
        ushort TcpPort,
        NetworkStream ControlStream,
        SemaphoreSlim WriteLock,
        DateTime RegisteredAtUtc)
    {
        public DateTime LastSeenUtc { get; set; } = RegisteredAtUtc;
    }

    private static readonly ConcurrentDictionary<ulong, RegisteredHost> Hosts = new();
    private static readonly ConcurrentDictionary<ulong, TaskCompletionSource<TcpClient>> PendingBridges = new();
    private static readonly SemaphoreSlim ClientSlots = new(256, 256);

    // A client must send its first frame within this time, or the connection is dropped (slowloris guard).
    private static TimeSpan _firstFrameTimeout = TimeSpan.FromSeconds(10);

    // Concurrent connections allowed per remote IP. Each registered host holds one and each bridged session two,
    // so this is generous for an office behind one NAT while stopping a single address from taking every slot.
    private static int _maxConnectionsPerIp = 64;
    private static readonly Dictionary<IPAddress, int> ConnectionsPerIp = new();
    private static long _totalLookups;
    private static long _activeBridges;

    public static async Task<int> Main(string[] args)
    {
        int port = DefaultRelayPort;
        bool selfTest = false;

        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--port" && i + 1 < args.Length && int.TryParse(args[i + 1], out int p))
            {
                port = p;
                i++;
            }
            else if (args[i] == "--self-test")
            {
                selfTest = true;
            }
        }

        using var cts = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            cts.Cancel();
        };

        var listener = new TcpListener(IPAddress.Any, port);
        listener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        listener.Start();

        int boundPort = ((IPEndPoint)listener.LocalEndpoint).Port;
        Log($"CppDesk .NET 10 Rendezvous & Relay Server listening on 0.0.0.0:{boundPort}");

        if (selfTest)
        {
            _ = Task.Run(() => AcceptLoopAsync(listener, cts.Token));
            bool ok = await RunSelfTestAsync(boundPort) && await RunHardeningSelfTestAsync(boundPort) && await RunOwnershipSelfTestAsync(boundPort);
            cts.Cancel();
            listener.Stop();
            return ok ? 0 : 1;
        }

        await AcceptLoopAsync(listener, cts.Token);
        listener.Stop();
        return 0;
    }

    private static async Task AcceptLoopAsync(TcpListener listener, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                TcpClient client = await listener.AcceptTcpClientAsync(ct);
                client.NoDelay = true;
                EnableKeepAlive(client);

                IPAddress? remoteIp = (client.Client.RemoteEndPoint as IPEndPoint)?.Address;
                if (remoteIp is not null && !TryAcquireIp(remoteIp))
                {
                    client.Dispose();
                    continue;
                }

                _ = Task.Run(async () =>
                {
                    try
                    {
                        if (!await ClientSlots.WaitAsync(100, ct))
                        {
                            client.Dispose();
                            return;
                        }
                        try
                        {
                            await HandleClientAsync(client, ct);
                        }
                        finally
                        {
                            ClientSlots.Release();
                        }
                    }
                    finally
                    {
                        if (remoteIp is not null) ReleaseIp(remoteIp);
                    }
                }, ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                // Ignore transient accept error
            }
        }
    }

    private static bool TryAcquireIp(IPAddress ip)
    {
        lock (ConnectionsPerIp)
        {
            ConnectionsPerIp.TryGetValue(ip, out int n);
            if (n >= _maxConnectionsPerIp) return false;
            ConnectionsPerIp[ip] = n + 1;
            return true;
        }
    }

    private static void ReleaseIp(IPAddress ip)
    {
        lock (ConnectionsPerIp)
        {
            if (!ConnectionsPerIp.TryGetValue(ip, out int n)) return;
            if (n <= 1) ConnectionsPerIp.Remove(ip);
            else ConnectionsPerIp[ip] = n - 1;
        }
    }

    // Detect peers that vanished without closing (bridged sessions have no other idle timeout).
    private static void EnableKeepAlive(TcpClient client)
    {
        try
        {
            Socket sock = client.Client;
            sock.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
            sock.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveTime, 60);
            sock.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveInterval, 15);
            sock.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveRetryCount, 4);
        }
        catch (Exception)
        {
            // Not supported on this platform: keep the default behaviour.
        }
    }

    private static async Task HandleClientAsync(TcpClient client, CancellationToken ct)
    {
        bool keepAlive = false;
        try
        {
            NetworkStream stream = client.GetStream();
            using var firstFrameCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            firstFrameCts.CancelAfter(_firstFrameTimeout);
            var (ok, type, _, payload) = await ReadFrameAsync(stream, firstFrameCts.Token);
            if (!ok) return;

            switch (type)
            {
                case PacketRelayRegister:
                {
                    int pos = 0;
                    ulong deskId = ReadU64(payload, ref pos);
                    string hostname = ReadString(payload, ref pos);
                    string ip = ReadString(payload, ref pos);
                    ushort tcpPort = ReadU16(payload, ref pos);

                    if (string.IsNullOrWhiteSpace(ip) && client.Client.RemoteEndPoint is IPEndPoint ep)
                    {
                        ip = ep.Address.ToString();
                    }

                    // Hosts from before ownership proofs end the frame here.
                    bool ownerCapable = pos < payload.Length && payload[pos] == OwnerCap;
                    if (ownerCapable)
                    {
                        byte[] nonce = RandomNumberGenerator.GetBytes(OwnerNonceLength);
                        bool enrolling = !HasLiveOwner(deskId);
                        byte[] challenge = new byte[1 + OwnerNonceLength];
                        challenge[0] = enrolling ? ModeEnroll : ModeProve;
                        nonce.CopyTo(challenge, 1);
                        await WriteFrameAsync(stream, PacketRelayRegisterChallenge, 0, challenge, ct);

                        var (pOk, pType, _, proof) = await ReadFrameAsync(stream, firstFrameCts.Token);
                        bool proven = pOk && pType == PacketRelayRegisterProof && proof.Length == OwnerKeyLength &&
                                      (enrolling ? EnrollOwner(deskId, proof) : VerifyOwner(deskId, nonce, proof));
                        if (!proven)
                        {
                            await WriteFrameAsync(stream, PacketRelayRegisterAck, 0, new byte[] { 0 }, ct);
                            return;
                        }

                        // The owner takes its registration over from its own stale connection.
                        if (Hosts.TryGetValue(deskId, out var stale))
                        {
                            try { stale.ControlStream.Dispose(); } catch { }
                        }
                    }
                    else if (HasLiveOwner(deskId) ||
                             (Hosts.TryGetValue(deskId, out var existing) &&
                              (DateTime.UtcNow - existing.LastSeenUtc) < TimeSpan.FromSeconds(45)))
                    {
                        await WriteFrameAsync(stream, PacketRelayRegisterAck, 0, new byte[] { 0 }, ct);
                        return;
                    }

                    var host = new RegisteredHost(deskId, hostname, ip, tcpPort, stream, new SemaphoreSlim(1, 1), DateTime.UtcNow);
                    Hosts[deskId] = host;

                    await WriteFrameAsync(stream, PacketRelayRegisterAck, 0, new byte[] { 1 }, ct);
                    Log($"Registered Host {FormatDeskId(deskId)} ({hostname} @ {ip}:{tcpPort}) | Active Hosts: {Hosts.Count}");

                    keepAlive = true;
                    using var pingCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
                    pingCts.CancelAfter(TimeSpan.FromSeconds(90));
                    try
                    {
                        while (!ct.IsCancellationRequested)
                        {
                            var (pOk, pType, _, _) = await ReadFrameAsync(stream, pingCts.Token);
                            if (!pOk) break;
                            host.LastSeenUtc = DateTime.UtcNow;
                            if (ownerCapable) TouchOwner(deskId);
                            pingCts.CancelAfter(TimeSpan.FromSeconds(90));
                            if (pType == PacketPing)
                            {
                                await host.WriteLock.WaitAsync(ct);
                                try
                                {
                                    await WriteFrameAsync(stream, PacketPong, 0, Array.Empty<byte>(), ct);
                                }
                                finally
                                {
                                    host.WriteLock.Release();
                                }
                            }
                        }
                    }
                    finally
                    {
                        if (ownerCapable) TouchOwner(deskId);
                        if (((ICollection<KeyValuePair<ulong, RegisteredHost>>)Hosts)
                                .Remove(new KeyValuePair<ulong, RegisteredHost>(deskId, host)))
                        {
                            host.WriteLock.Dispose();
                        }
                        client.Dispose();
                        Log($"Unregistered Host {FormatDeskId(deskId)} | Active Hosts: {Hosts.Count}");
                    }
                    break;
                }

                case PacketRelayLookup:
                {
                    Interlocked.Increment(ref _totalLookups);
                    int pos = 0;
                    ulong targetId = ReadU64(payload, ref pos);

                    using var ms = new MemoryStream();
                    if (Hosts.TryGetValue(targetId, out RegisteredHost? foundHost))
                    {
                        if ((DateTime.UtcNow - foundHost.LastSeenUtc) > TimeSpan.FromSeconds(90))
                        {
                            if (((ICollection<KeyValuePair<ulong, RegisteredHost>>)Hosts)
                                    .Remove(new KeyValuePair<ulong, RegisteredHost>(targetId, foundHost)))
                            {
                                foundHost.WriteLock.Dispose();
                                try { foundHost.ControlStream.Dispose(); } catch { }
                            }
                            foundHost = null;
                        }
                    }

                    if (foundHost is not null)
                    {
                        ms.WriteByte(1);
                        WriteString(ms, foundHost.Ip);
                        WriteU16(ms, foundHost.TcpPort);
                        WriteString(ms, foundHost.Hostname);
                    }
                    else
                    {
                        ms.WriteByte(0);
                        WriteString(ms, string.Empty);
                        WriteU16(ms, 0);
                        WriteString(ms, string.Empty);
                    }

                    await WriteFrameAsync(stream, PacketRelayLookupResp, 0, ms.ToArray(), ct);
                    break;
                }

                case PacketRelayConnectReq:
                {
                    int pos = 0;
                    ulong targetId = ReadU64(payload, ref pos);

                    if (!Hosts.TryGetValue(targetId, out RegisteredHost? host))
                    {
                        await WriteFrameAsync(stream, PacketRelayBridgeReady, 0, new byte[] { 0 }, ct);
                        break;
                    }

                    ulong token = BinaryPrimitives.ReadUInt64LittleEndian(RandomNumberGenerator.GetBytes(8));
                    var tcs = new TaskCompletionSource<TcpClient>(TaskCreationOptions.RunContinuationsAsynchronously);
                    PendingBridges[token] = tcs;

                    byte[] reqPayload = new byte[8];
                    BinaryPrimitives.WriteUInt64LittleEndian(reqPayload, token);
                    await host.WriteLock.WaitAsync(ct);
                    try
                    {
                        await WriteFrameAsync(host.ControlStream, PacketRelayIncomingReq, 0, reqPayload, ct);
                    }
                    finally
                    {
                        host.WriteLock.Release();
                    }

                    using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
                    timeoutCts.CancelAfter(TimeSpan.FromMilliseconds(3500));

                    TcpClient? hostBridgeClient = null;
                    try
                    {
                        hostBridgeClient = await tcs.Task.WaitAsync(timeoutCts.Token);
                    }
                    catch
                    {
                        if (PendingBridges.TryRemove(token, out _))
                        {
                            tcs.TrySetCanceled();
                        }
                        else
                        {
                            try
                            {
                                var orphan = await tcs.Task;
                                orphan?.Dispose();
                            }
                            catch { }
                        }
                    }

                    if (hostBridgeClient is null)
                    {
                        await WriteFrameAsync(stream, PacketRelayBridgeReady, 0, new byte[] { 0 }, ct);
                        break;
                    }

                    await WriteFrameAsync(stream, PacketRelayBridgeReady, 0, new byte[] { 1 }, ct);

                    keepAlive = true;
                    Interlocked.Increment(ref _activeBridges);
                    Log($"Bridged Relay Session for {FormatDeskId(targetId)} (Active Bridges: {Interlocked.Read(ref _activeBridges)})");

                    try
                    {
                        NetworkStream hostBridgeStream = hostBridgeClient.GetStream();
                        using var linked = CancellationTokenSource.CreateLinkedTokenSource(ct);
                        Task t1 = stream.CopyToAsync(hostBridgeStream, linked.Token);
                        Task t2 = hostBridgeStream.CopyToAsync(stream, linked.Token);
                        await Task.WhenAny(t1, t2);
                        linked.Cancel();
                        try { await Task.WhenAll(t1, t2); } catch (OperationCanceledException) { } catch (IOException) { }
                    }
                    finally
                    {
                        Interlocked.Decrement(ref _activeBridges);
                        hostBridgeClient.Dispose();
                        client.Dispose();
                    }
                    break;
                }

                case PacketRelayBridgeAccept:
                {
                    int pos = 0;
                    ulong token = ReadU64(payload, ref pos);
                    if (PendingBridges.TryRemove(token, out var tcs))
                    {
                        keepAlive = true;
                        tcs.TrySetResult(client);
                    }
                    break;
                }
            }
        }
        catch
        {
            // Connection closed
        }
        finally
        {
            if (!keepAlive)
            {
                client.Dispose();
            }
        }
    }

    private static async Task<(bool Ok, byte Type, byte Flags, byte[] Payload)> ReadFrameAsync(NetworkStream stream, CancellationToken ct)
    {
        byte[] hdr = new byte[10];
        if (!await ReadExactAsync(stream, hdr, ct)) return (false, 0, 0, Array.Empty<byte>());

        uint magic = BinaryPrimitives.ReadUInt32LittleEndian(hdr.AsSpan(0, 4));
        if (magic != ProtocolMagic) return (false, 0, 0, Array.Empty<byte>());

        byte type = hdr[4];
        byte flags = hdr[5];
        uint size = BinaryPrimitives.ReadUInt32LittleEndian(hdr.AsSpan(6, 4));
        uint cap = type switch
        {
            PacketRelayRegister or PacketRelayLookup or PacketRelayConnectReq or PacketRelayBridgeAccept or PacketRelayRegisterProof => MaxControlFrame,
            _ => MaxPayloadSize
        };
        if (size > cap) return (false, 0, 0, Array.Empty<byte>());

        byte[] payload = size > 0 ? new byte[size] : Array.Empty<byte>();
        if (size > 0 && !await ReadExactAsync(stream, payload, ct))
        {
            return (false, 0, 0, Array.Empty<byte>());
        }
        return (true, type, flags, payload);
    }

    private static async Task<bool> ReadExactAsync(NetworkStream stream, byte[] buffer, CancellationToken ct)
    {
        int offset = 0;
        while (offset < buffer.Length)
        {
            int n = await stream.ReadAsync(buffer.AsMemory(offset, buffer.Length - offset), ct);
            if (n <= 0) return false;
            offset += n;
        }
        return true;
    }

    private static async Task WriteFrameAsync(NetworkStream stream, byte type, byte flags, byte[] payload, CancellationToken ct)
    {
        byte[] frame = new byte[10 + payload.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(0, 4), ProtocolMagic);
        frame[4] = type;
        frame[5] = flags;
        BinaryPrimitives.WriteUInt32LittleEndian(frame.AsSpan(6, 4), (uint)payload.Length);
        if (payload.Length > 0)
        {
            Buffer.BlockCopy(payload, 0, frame, 10, payload.Length);
        }
        await stream.WriteAsync(frame, ct);
    }

    private static ushort ReadU16(byte[] buf, ref int pos)
    {
        ushort v = BinaryPrimitives.ReadUInt16LittleEndian(buf.AsSpan(pos, 2));
        pos += 2;
        return v;
    }

    private static ulong ReadU64(byte[] buf, ref int pos)
    {
        ulong v = BinaryPrimitives.ReadUInt64LittleEndian(buf.AsSpan(pos, 8));
        pos += 8;
        return v;
    }

    private static string ReadString(byte[] buf, ref int pos)
    {
        ushort len = ReadU16(buf, ref pos);
        string s = Encoding.UTF8.GetString(buf, pos, len);
        pos += len;
        return s;
    }

    private static void WriteU16(MemoryStream ms, ushort val)
    {
        Span<byte> b = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16LittleEndian(b, val);
        ms.Write(b);
    }

    private static void WriteU64(MemoryStream ms, ulong val)
    {
        Span<byte> b = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(b, val);
        ms.Write(b);
    }

    private static void WriteString(MemoryStream ms, string s)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(s);
        ushort len = (ushort)Math.Min(bytes.Length, 65535);
        WriteU16(ms, len);
        ms.Write(bytes, 0, len);
    }

    private static string FormatDeskId(ulong id)
    {
        string s = id.ToString("D9");
        return s.Length == 9 ? $"{s[..3]} {s[3..6]} {s[6..9]}" : s;
    }

    private static void Log(string msg)
    {
        Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {msg}");
    }

    // True when the server closed the connection (EOF or reset); false if it is still open after `wait`.
    private static async Task<bool> IsClosedByServerAsync(TcpClient client, TimeSpan wait)
    {
        using var cts = new CancellationTokenSource(wait);
        try
        {
            byte[] buf = new byte[1];
            return await client.GetStream().ReadAsync(buf, cts.Token) == 0;
        }
        catch (OperationCanceledException)
        {
            return false;
        }
        catch (Exception ex) when (ex is IOException or SocketException or ObjectDisposedException)
        {
            return true;
        }
    }

    private static async Task<bool> RunHardeningSelfTestAsync(int port)
    {
        TimeSpan savedTimeout = _firstFrameTimeout;
        int savedMax = _maxConnectionsPerIp;
        var open = new List<TcpClient>();
        try
        {
            // 1. A client that never sends its first frame is dropped after the timeout.
            _firstFrameTimeout = TimeSpan.FromMilliseconds(400);
            using (var silent = new TcpClient())
            {
                await silent.ConnectAsync(IPAddress.Loopback, port);
                if (!await IsClosedByServerAsync(silent, TimeSpan.FromSeconds(3)))
                {
                    Log("Self-test FAILED: silent connection was not dropped");
                    return false;
                }
            }

            // 2. One IP cannot exceed the per-IP limit, and capacity returns when connections close.
            _firstFrameTimeout = TimeSpan.FromSeconds(30);
            _maxConnectionsPerIp = 3;
            await Task.Delay(300); // let the connections from the earlier self-test release their slots
            for (int i = 0; i < 3; i++)
            {
                var c = new TcpClient();
                open.Add(c);
                await c.ConnectAsync(IPAddress.Loopback, port);
            }

            using (var extra = new TcpClient())
            {
                await extra.ConnectAsync(IPAddress.Loopback, port);
                if (!await IsClosedByServerAsync(extra, TimeSpan.FromSeconds(2)))
                {
                    Log("Self-test FAILED: connection over the per-IP limit was accepted");
                    return false;
                }
            }
            if (await IsClosedByServerAsync(open[0], TimeSpan.FromMilliseconds(300)))
            {
                Log("Self-test FAILED: a connection within the limit was dropped");
                return false;
            }

            foreach (var c in open) c.Dispose();
            open.Clear();
            await Task.Delay(500);
            using (var again = new TcpClient())
            {
                await again.ConnectAsync(IPAddress.Loopback, port);
                if (await IsClosedByServerAsync(again, TimeSpan.FromMilliseconds(500)))
                {
                    Log("Self-test FAILED: per-IP capacity was not released");
                    return false;
                }
            }

            Log("Self-test verified: first-frame timeout and per-IP limit");
            return true;
        }
        finally
        {
            foreach (var c in open) c.Dispose();
            _firstFrameTimeout = savedTimeout;
            _maxConnectionsPerIp = savedMax;
        }
    }

    private static byte[] ComputeOwnerProof(byte[] key, ulong deskId, byte[] nonce)
    {
        byte[] msg = new byte[OwnerLabel.Length + 8 + nonce.Length];
        OwnerLabel.CopyTo(msg, 0);
        BinaryPrimitives.WriteUInt64LittleEndian(msg.AsSpan(OwnerLabel.Length, 8), deskId);
        nonce.CopyTo(msg, OwnerLabel.Length + 8);
        return HMACSHA256.HashData(key, msg);
    }

    private static bool HasLiveOwner(ulong deskId)
    {
        lock (Owners) return LiveOwnerLocked(deskId) is not null;
    }

    private static OwnerRecord? LiveOwnerLocked(ulong deskId)
    {
        if (!Owners.TryGetValue(deskId, out var rec)) return null;
        if (DateTime.UtcNow - rec.LastSeenUtc > OwnerTtl)
        {
            Owners.Remove(deskId);
            return null;
        }
        return rec;
    }

    // False when the ID already has a live owner. A full table accepts the ID unprotected rather than lock real hosts out.
    private static bool EnrollOwner(ulong deskId, byte[] key)
    {
        lock (Owners)
        {
            if (LiveOwnerLocked(deskId) is not null) return false;
            if (Owners.Count < OwnerMaxRecords || Owners.ContainsKey(deskId))
            {
                Owners[deskId] = new OwnerRecord(key, DateTime.UtcNow);
            }
            return true;
        }
    }

    private static bool VerifyOwner(ulong deskId, byte[] nonce, byte[] proof)
    {
        lock (Owners)
        {
            var rec = LiveOwnerLocked(deskId);
            if (rec is null) return false;
            if (!CryptographicOperations.FixedTimeEquals(ComputeOwnerProof(rec.Key, deskId, nonce), proof)) return false;
            Owners[deskId] = rec with { LastSeenUtc = DateTime.UtcNow };
            return true;
        }
    }

    private static void TouchOwner(ulong deskId)
    {
        lock (Owners)
        {
            if (LiveOwnerLocked(deskId) is { } rec) Owners[deskId] = rec with { LastSeenUtc = DateTime.UtcNow };
        }
    }

    private static async Task<bool> RunOwnershipSelfTestAsync(int port)
    {
        const ulong deskId = 731904286UL;
        byte[] key = new byte[OwnerKeyLength];
        Array.Fill(key, (byte)0x5A);

        async Task<(TcpClient Client, NetworkStream Stream)> Dial()
        {
            var c = new TcpClient();
            await c.ConnectAsync(IPAddress.Loopback, port);
            return (c, c.GetStream());
        }

        static byte[] RegisterPayload(ulong id, string host, bool ownerCap)
        {
            using var ms = new MemoryStream();
            WriteU64(ms, id);
            WriteString(ms, host);
            WriteString(ms, "127.0.0.1");
            WriteU16(ms, 50991);
            if (ownerCap) ms.WriteByte(OwnerCap);
            return ms.ToArray();
        }

        // Sends REGISTER and returns (challenge mode, nonce) or (-1, ack byte) when the relay answered with an ack.
        async Task<(int Mode, byte[] Data)> Register(NetworkStream s, ulong id, string host, bool ownerCap)
        {
            await WriteFrameAsync(s, PacketRelayRegister, 0, RegisterPayload(id, host, ownerCap), CancellationToken.None);
            var (ok, type, _, pay) = await ReadFrameAsync(s, CancellationToken.None);
            if (!ok) return (-2, Array.Empty<byte>());
            if (type == PacketRelayRegisterChallenge && pay.Length == 1 + OwnerNonceLength) return (pay[0], pay[1..]);
            if (type == PacketRelayRegisterAck && pay.Length >= 1) return (-1, new[] { pay[0] });
            return (-2, Array.Empty<byte>());
        }

        async Task<int> Answer(NetworkStream s, byte[] value)
        {
            await WriteFrameAsync(s, PacketRelayRegisterProof, 0, value, CancellationToken.None);
            var (ok, type, _, pay) = await ReadFrameAsync(s, CancellationToken.None);
            return ok && type == PacketRelayRegisterAck && pay.Length >= 1 ? pay[0] : -2;
        }

        async Task<string> Lookup(ulong id)
        {
            var (c, s) = await Dial();
            using (c)
            {
                using var ms = new MemoryStream();
                WriteU64(ms, id);
                await WriteFrameAsync(s, PacketRelayLookup, 0, ms.ToArray(), CancellationToken.None);
                var (ok, type, _, pay) = await ReadFrameAsync(s, CancellationToken.None);
                if (!ok || type != PacketRelayLookupResp) return "<no reply>";
                int pos = 0;
                byte found = pay[pos++];
                ReadString(pay, ref pos);
                ReadU16(pay, ref pos);
                string name = ReadString(pay, ref pos);
                return found == 1 ? name : "";
            }
        }

        bool Fail(string why)
        {
            Log($"Self-test FAILED: desk-ID ownership: {why}");
            return false;
        }

        // The owner's first registration enrolls its key.
        var (ownerClient, ownerStream) = await Dial();
        using (ownerClient)
        {
            var first = await Register(ownerStream, deskId, "owner-host", true);
            if (first.Mode != ModeEnroll) return Fail("first registration was not asked to enroll");
            if (await Answer(ownerStream, key) != 1) return Fail("enrollment was refused");
            if (await Lookup(deskId) != "owner-host") return Fail("enrolled host not found");

            // A host that sends no proof cannot take the ID.
            var (c1, s1) = await Dial();
            using (c1)
            {
                var r = await Register(s1, deskId, "evil-legacy", false);
                if (r.Mode != -1 || r.Data[0] != 0) return Fail("proof-less registration was accepted");
            }

            // A wrong proof and the owner's earlier proof replayed against a new nonce are refused.
            byte[] earlierProof = ComputeOwnerProof(key, deskId, first.Data);
            var (c2, s2) = await Dial();
            using (c2)
            {
                var r = await Register(s2, deskId, "evil-forger", true);
                if (r.Mode != ModeProve) return Fail("owned ID was not challenged to prove");
                if (await Answer(s2, new byte[OwnerKeyLength]) != 0) return Fail("wrong proof was accepted");
            }
            var (c3, s3) = await Dial();
            using (c3)
            {
                var r = await Register(s3, deskId, "evil-replay", true);
                if (r.Mode != ModeProve) return Fail("owned ID was not challenged to prove (replay)");
                if (await Answer(s3, earlierProof) != 0) return Fail("replayed proof was accepted");
            }
            if (await Lookup(deskId) != "owner-host") return Fail("a failed attempt displaced the owner");

            // The owner comes back and takes its registration over from its own stale connection.
            var (c4, s4) = await Dial();
            using (c4)
            {
                var r = await Register(s4, deskId, "owner-host-2", true);
                if (r.Mode != ModeProve) return Fail("owner was not challenged to prove");
                if (await Answer(s4, ComputeOwnerProof(key, deskId, r.Data)) != 1) return Fail("owner's valid proof was refused");
                if (await Lookup(deskId) != "owner-host-2") return Fail("owner did not take the registration back");
            }
        }

        // With the owner offline, nobody else can take its ID by registering first.
        await Task.Delay(300);
        if (await Lookup(deskId) != "") return Fail("owner should be offline by now");
        var (c6, s6) = await Dial();
        using (c6)
        {
            var r = await Register(s6, deskId, "squatter-legacy", false);
            if (r.Mode != -1 || r.Data[0] != 0) return Fail("proof-less host took an offline owner's ID");
        }
        var (c7, s7) = await Dial();
        using (c7)
        {
            var r = await Register(s7, deskId, "squatter-capable", true);
            if (r.Mode != ModeProve) return Fail("offline owner's ID was offered for enrollment");
        }

        // A host from before this feature still registers an ID nobody owns.
        var (c5, s5) = await Dial();
        using (c5)
        {
            var r = await Register(s5, 731904287UL, "legacy-host", false);
            if (r.Mode != -1 || r.Data[0] != 1) return Fail("legacy host could not register an unowned ID");
            if (await Lookup(731904287UL) != "legacy-host") return Fail("legacy host not found");
        }

        Log("Self-test verified: desk-ID ownership (enroll, prove, refuse hijack)");
        return true;
    }

    private static async Task<bool> RunSelfTestAsync(int port)
    {
        using var hostClient = new TcpClient();
        await hostClient.ConnectAsync(IPAddress.Loopback, port);
        NetworkStream hStream = hostClient.GetStream();

        using var regMs = new MemoryStream();
        WriteU64(regMs, 482910375UL);
        WriteString(regMs, "DotNet-TestHost");
        WriteString(regMs, "127.0.0.1");
        WriteU16(regMs, 50990);

        await WriteFrameAsync(hStream, PacketRelayRegister, 0, regMs.ToArray(), CancellationToken.None);
        var (ackOk, ackType, _, ackPay) = await ReadFrameAsync(hStream, CancellationToken.None);
        if (!ackOk || ackType != PacketRelayRegisterAck || ackPay.Length < 1 || ackPay[0] != 1)
        {
            return false;
        }

        using var lookupClient = new TcpClient();
        await lookupClient.ConnectAsync(IPAddress.Loopback, port);
        NetworkStream lStream = lookupClient.GetStream();

        using var lookMs = new MemoryStream();
        WriteU64(lookMs, 482910375UL);
        await WriteFrameAsync(lStream, PacketRelayLookup, 0, lookMs.ToArray(), CancellationToken.None);

        var (respOk, respType, _, respPay) = await ReadFrameAsync(lStream, CancellationToken.None);
        if (!respOk || respType != PacketRelayLookupResp) return false;

        int pos = 0;
        byte found = respPay[pos++];
        string ip = ReadString(respPay, ref pos);
        ushort hostPort = ReadU16(respPay, ref pos);
        string hostName = ReadString(respPay, ref pos);

        Log($"Self-test verified: Found={found}, Host={hostName}, Endpoint={ip}:{hostPort}");
        return found == 1 && ip == "127.0.0.1" && hostPort == 50990 && hostName == "DotNet-TestHost";
    }
}
