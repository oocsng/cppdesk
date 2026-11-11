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
            bool ok = await RunSelfTestAsync(boundPort) && await RunHardeningSelfTestAsync(boundPort);
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

                    if (Hosts.TryGetValue(deskId, out var existing) &&
                        (DateTime.UtcNow - existing.LastSeenUtc) < TimeSpan.FromSeconds(45))
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
            PacketRelayRegister or PacketRelayLookup or PacketRelayConnectReq or PacketRelayBridgeAccept => MaxControlFrame,
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
