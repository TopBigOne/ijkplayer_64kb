package tv.danmaku.ijk.media.example.application;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.TextUtils;
import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * 极简 SNTP 校时，用于端到端延迟测量：offset = NTP 时间 - System.currentTimeMillis()。
 * 推流端（tools/latency/flv_sei_inject.py）向同一台 NTP 服务器校时，两边误差一般在 10ms 以内。
 *
 * 结果按服务器缓存 10 分钟；查询在后台线程里做，回调在主线程。
 */
public final class NtpClock {
    private static final String TAG = "NtpClock";
    private static final long CACHE_MS = 10 * 60 * 1000;
    private static final int TIMEOUT_MS = 3000;
    private static final long NTP_EPOCH_OFFSET_MS = 2208988800000L;   // 1900-01-01 到 1970-01-01

    public interface Callback {
        void onOffset(long offsetMs, long rttMs);
    }

    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static String sServer;
    private static long sOffsetMs;
    private static long sRttMs;
    private static long sUpdatedAt;   // SystemClock.elapsedRealtime()，0 表示没有结果
    private static boolean sQuerying;

    private NtpClock() {
    }

    /** 已有该服务器的有效结果返回 true */
    public static synchronized boolean hasOffset(String server) {
        return sUpdatedAt != 0 && TextUtils.equals(server, sServer)
                && SystemClock.elapsedRealtime() - sUpdatedAt < CACHE_MS;
    }

    public static synchronized long getOffsetMs() {
        return sOffsetMs;
    }

    public static synchronized long getRttMs() {
        return sRttMs;
    }

    public static synchronized String getServer() {
        return sUpdatedAt != 0 ? sServer : null;
    }

    /** 有缓存直接回调；否则后台查询，成功后回调（失败不回调） */
    public static void request(final String server, final Callback callback) {
        if (TextUtils.isEmpty(server))
            return;
        synchronized (NtpClock.class) {
            if (hasOffset(server)) {
                final long offset = sOffsetMs, rtt = sRttMs;
                sMain.post(() -> callback.onOffset(offset, rtt));
                return;
            }
            if (sQuerying)
                return;
            sQuerying = true;
        }
        new Thread(() -> {
            long[] r = null;
            // 取 3 次里 RTT 最小的一次，RTT 越小，网络不对称带来的误差越小
            for (int i = 0; i < 3; i++) {
                long[] one = query(server);
                if (one != null && (r == null || one[1] < r[1]))
                    r = one;
            }
            final long[] best = r;
            synchronized (NtpClock.class) {
                sQuerying = false;
                if (best != null) {
                    sServer = server;
                    sOffsetMs = best[0];
                    sRttMs = best[1];
                    sUpdatedAt = SystemClock.elapsedRealtime();
                }
            }
            if (best != null) {
                Log.i(TAG, "server " + server + " offset " + best[0] + " ms, rtt " + best[1] + " ms");
                sMain.post(() -> callback.onOffset(best[0], best[1]));
            }
        }, "NtpClock").start();
    }

    /** 返回 {offsetMs, rttMs}，失败返回 null */
    private static long[] query(String server) {
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setSoTimeout(TIMEOUT_MS);
            InetAddress address = InetAddress.getByName(server);
            byte[] buf = new byte[48];
            buf[0] = 0x1B;   // LI=0, VN=3, Mode=3(client)

            long t1 = System.currentTimeMillis();
            long t1Mono = SystemClock.elapsedRealtime();
            writeTimestamp(buf, 40, t1);   // transmit timestamp
            socket.send(new DatagramPacket(buf, buf.length, address, 123));

            DatagramPacket response = new DatagramPacket(buf, buf.length);
            socket.receive(response);
            long t4 = t1 + (SystemClock.elapsedRealtime() - t1Mono);

            int mode = buf[0] & 0x7;
            int stratum = buf[1] & 0xff;
            if (mode != 4 || stratum == 0 || stratum > 15)
                return null;
            long t2 = readTimestamp(buf, 32);   // receive timestamp
            long t3 = readTimestamp(buf, 40);   // transmit timestamp
            long offset = ((t2 - t1) + (t3 - t4)) / 2;
            long rtt = (t4 - t1) - (t3 - t2);
            return new long[]{offset, rtt};
        } catch (Exception e) {
            Log.w(TAG, "query " + server + " failed: " + e);
            return null;
        }
    }

    private static long readTimestamp(byte[] b, int off) {
        long seconds = 0, fraction = 0;
        for (int i = 0; i < 4; i++)
            seconds = (seconds << 8) | (b[off + i] & 0xff);
        for (int i = 4; i < 8; i++)
            fraction = (fraction << 8) | (b[off + i] & 0xff);
        return seconds * 1000 + fraction * 1000 / 0x100000000L - NTP_EPOCH_OFFSET_MS;
    }

    private static void writeTimestamp(byte[] b, int off, long unixMs) {
        long ntpMs = unixMs + NTP_EPOCH_OFFSET_MS;
        long seconds = ntpMs / 1000;
        long fraction = (ntpMs % 1000) * 0x100000000L / 1000;
        for (int i = 3; i >= 0; i--) {
            b[off + i] = (byte) (seconds & 0xff);
            seconds >>= 8;
        }
        for (int i = 7; i >= 4; i--) {
            b[off + i] = (byte) (fraction & 0xff);
            fraction >>= 8;
        }
    }
}
