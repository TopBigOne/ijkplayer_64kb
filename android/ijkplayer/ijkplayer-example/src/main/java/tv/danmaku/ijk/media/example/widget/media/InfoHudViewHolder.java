package tv.danmaku.ijk.media.example.widget.media;

import android.content.Context;
import android.os.Handler;
import android.os.Message;
import android.util.SparseArray;
import android.view.View;
import android.widget.TableLayout;
import android.widget.TextView;

import androidx.core.content.ContextCompat;

import java.util.Locale;

import tv.danmaku.ijk.media.player.IMediaPlayer;
import tv.danmaku.ijk.media.player.IjkMediaPlayer;
import tv.danmaku.ijk.media.player.MediaPlayerProxy;
import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.example.application.NtpClock;
import tv.danmaku.ijk.media.example.application.Settings;

public class InfoHudViewHolder {
    private final Context mContext;
    private TableLayoutBinder mTableLayoutBinder;
    private SparseArray<View> mRowMap = new SparseArray<View>();
    private IMediaPlayer mMediaPlayer;
    private long mLoadCost = 0;
    private long mSeekCost = 0;
    private int mLatencyTargetMs = 300;

    public InfoHudViewHolder(Context context, TableLayout tableLayout) {
        mContext = context;
        mTableLayoutBinder = new TableLayoutBinder(context, tableLayout);
    }

    private void appendSection(int nameId) {
        mTableLayoutBinder.appendSection(nameId);
    }

    private void appendRow(int nameId) {
        View rowView = mTableLayoutBinder.appendRow2(nameId, null);
        applyHudTextColor(rowView);
        mRowMap.put(nameId, rowView);
    }

    private void setRowValue(int id, String value) {
        View rowView = mRowMap.get(id);
        if (rowView == null) {
            rowView = mTableLayoutBinder.appendRow2(id, value);
            applyHudTextColor(rowView);
            mRowMap.put(id, rowView);
        } else {
            mTableLayoutBinder.setValueText(rowView, value);
        }
    }

    /** HUD 是浅肉色底，行布局与「媒体信息」弹窗共用，所以文字颜色只在这里单独设置。 */
    private void applyHudTextColor(View rowView) {
        TextView name = rowView.findViewById(R.id.name);
        TextView value = rowView.findViewById(R.id.value);
        if (name != null)
            name.setTextColor(ContextCompat.getColor(mContext, R.color.hud_text_name));
        if (value != null)
            value.setTextColor(ContextCompat.getColor(mContext, R.color.hud_text_value));
    }

    /** 设置值并按 warn 标红（延迟超过阈值） */
    private void setRowValue(int id, String value, boolean warn) {
        setRowValue(id, value);
        TextView valueView = mRowMap.get(id).findViewById(R.id.value);
        if (valueView != null)
            valueView.setTextColor(ContextCompat.getColor(mContext, warn ? R.color.hud_text_warn : R.color.hud_text_value));
    }

    /** 直播延迟相关的几行，放在 HUD 最上面 */
    private void updateLatencyRows(IjkMediaPlayer mp) {
        // 端到端：推流端 SEI 时间戳 -> 本机送显
        long e2e = mp.getE2eLatency();
        if (e2e >= 0)
            setRowValue(R.string.e2e_latency, String.format(Locale.US, "%d ms", e2e), e2e > mLatencyTargetMs);
        else
            setRowValue(R.string.e2e_latency, mContext.getString(R.string.e2e_latency_no_sei), false);

        // 到达延迟：推流 -> 服务器 -> 网络，端到端减去它就是播放端耗时
        long recv = mp.getE2eRecvLatency();
        if (recv >= 0 && e2e >= 0)
            setRowValue(R.string.recv_latency, mContext.getString(R.string.recv_latency_fmt, recv, e2e - recv), false);
        else
            setRowValue(R.string.recv_latency, "-", false);

        // 播放端缓冲：包队列里音视频较大的那个
        long buffered = Math.max(mp.getVideoCachedDuration(), mp.getAudioCachedDuration());
        setRowValue(R.string.buffer_latency, formatedDurationMilli(buffered), buffered > mLatencyTargetMs);

        String chase = mp.isLiveChasing()
                ? mContext.getString(R.string.live_chasing)
                : mContext.getString(R.string.live_not_chasing);
        long drops = mp.getLiveDropCount();
        if (drops > 0)
            chase += mContext.getString(R.string.live_drop_count_fmt, drops);
        setRowValue(R.string.live_chase_state, chase);

        String server = NtpClock.getServer();
        if (server != null)
            setRowValue(R.string.clock_offset, String.format(Locale.US, "%+d ms (rtt %d ms)",
                    NtpClock.getOffsetMs(), NtpClock.getRttMs()));
        else
            setRowValue(R.string.clock_offset, mContext.getString(R.string.clock_offset_none));
    }

    public void setMediaPlayer(IMediaPlayer mp) {
        mMediaPlayer = mp;
        mLatencyTargetMs = new Settings(mContext).getLatencyTargetMs();
        if (mMediaPlayer != null) {
            mHandler.sendEmptyMessageDelayed(MSG_UPDATE_HUD, 500);
        } else {
            mHandler.removeMessages(MSG_UPDATE_HUD);
        }
    }

    private static String formatedDurationMilli(long duration) {
        if (duration >=  1000) {
            return String.format(Locale.US, "%.2f s", ((float)duration) / 1000);
        } else {
            return String.format(Locale.US, "%d ms", duration);
        }
    }

    private static String formatedSpeed(long bytes,long elapsed_milli) {
        if (elapsed_milli <= 0) {
            return "0 B/s";
        }

        if (bytes <= 0) {
            return "0 B/s";
        }

        float bytes_per_sec = ((float)bytes) * 1000.f /  elapsed_milli;
        if (bytes_per_sec >= 1000 * 1000) {
            return String.format(Locale.US, "%.2f MB/s", ((float)bytes_per_sec) / 1000 / 1000);
        } else if (bytes_per_sec >= 1000) {
            return String.format(Locale.US, "%.1f KB/s", ((float)bytes_per_sec) / 1000);
        } else {
            return String.format(Locale.US, "%d B/s", (long)bytes_per_sec);
        }
    }

    public void updateLoadCost(long time)  {
        mLoadCost = time;
    }

    public void updateSeekCost(long time)  {
        mSeekCost = time;
    }

    private static String formatedSize(long bytes) {
        if (bytes >= 100 * 1000) {
            return String.format(Locale.US, "%.2f MB", ((float)bytes) / 1000 / 1000);
        } else if (bytes >= 100) {
            return String.format(Locale.US, "%.1f KB", ((float)bytes) / 1000);
        } else {
            return String.format(Locale.US, "%d B", bytes);
        }
    }

    private static final int MSG_UPDATE_HUD = 1;
    private Handler mHandler = new Handler() {
        @Override
        public void handleMessage(Message msg) {
            switch (msg.what) {
                case MSG_UPDATE_HUD: {
                    InfoHudViewHolder holder = InfoHudViewHolder.this;
                    IjkMediaPlayer mp = null;
                    if (mMediaPlayer == null)
                        break;
                    if (mMediaPlayer instanceof IjkMediaPlayer) {
                        mp = (IjkMediaPlayer) mMediaPlayer;
                    } else if (mMediaPlayer instanceof MediaPlayerProxy) {
                        MediaPlayerProxy proxy = (MediaPlayerProxy) mMediaPlayer;
                        IMediaPlayer internal = proxy.getInternalMediaPlayer();
                        if (internal != null && internal instanceof IjkMediaPlayer)
                            mp = (IjkMediaPlayer) internal;
                    }
                    if (mp == null)
                        break;

                    updateLatencyRows(mp);

                    int vdec = mp.getVideoDecoder();
                    switch (vdec) {
                        case IjkMediaPlayer.FFP_PROPV_DECODER_AVCODEC:
                            setRowValue(R.string.vdec, mContext.getString(R.string.vdec_avcodec));
                            break;
                        case IjkMediaPlayer.FFP_PROPV_DECODER_MEDIACODEC:
                            setRowValue(R.string.vdec, mContext.getString(R.string.vdec_mediacodec));
                            break;
                        default:
                            setRowValue(R.string.vdec, "");
                            break;
                    }

                    float fpsOutput = mp.getVideoOutputFramesPerSecond();
                    float fpsDecode = mp.getVideoDecodeFramesPerSecond();
                    setRowValue(R.string.fps, String.format(Locale.US, "%.2f / %.2f", fpsDecode, fpsOutput));

                    long videoCachedDuration = mp.getVideoCachedDuration();
                    long audioCachedDuration = mp.getAudioCachedDuration();
                    long videoCachedBytes    = mp.getVideoCachedBytes();
                    long audioCachedBytes    = mp.getAudioCachedBytes();
                    long tcpSpeed            = mp.getTcpSpeed();
                    long bitRate             = mp.getBitRate();
                    long seekLoadDuration    = mp.getSeekLoadDuration();

                    setRowValue(R.string.v_cache, String.format(Locale.US, "%s, %s", formatedDurationMilli(videoCachedDuration), formatedSize(videoCachedBytes)));
                    setRowValue(R.string.a_cache, String.format(Locale.US, "%s, %s", formatedDurationMilli(audioCachedDuration), formatedSize(audioCachedBytes)));
                    setRowValue(R.string.load_cost, String.format(Locale.US, "%d ms", mLoadCost));
                    setRowValue(R.string.seek_cost, String.format(Locale.US, "%d ms", mSeekCost));
                    setRowValue(R.string.seek_load_cost, String.format(Locale.US, "%d ms", seekLoadDuration));
                    setRowValue(R.string.tcp_speed, String.format(Locale.US, "%s", formatedSpeed(tcpSpeed, 1000)));
                    setRowValue(R.string.bit_rate, String.format(Locale.US, "%.2f kbps", bitRate/1000f));

                    mHandler.removeMessages(MSG_UPDATE_HUD);
                    mHandler.sendEmptyMessageDelayed(MSG_UPDATE_HUD, 500);
                }
            }
        }
    };
}
