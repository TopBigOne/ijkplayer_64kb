/*
 * Copyright (C) 2015 Bilibili
 * Copyright (C) 2015 Zhang Rui <bbcallen@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package tv.danmaku.ijk.media.example.application;

import android.content.Context;
import android.content.SharedPreferences;
import android.preference.PreferenceManager;

import tv.danmaku.ijk.media.example.R;

public class Settings {
    private Context mAppContext;
    private SharedPreferences mSharedPreferences;

    public static final int PV_PLAYER__Auto = 0;
    public static final int PV_PLAYER__AndroidMediaPlayer = 1;
    public static final int PV_PLAYER__IjkMediaPlayer = 2;
    public static final int PV_PLAYER__IjkExoMediaPlayer = 3;

    public Settings(Context context) {
        mAppContext = context.getApplicationContext();
        mSharedPreferences = PreferenceManager.getDefaultSharedPreferences(mAppContext);
    }

    public boolean getEnableBackgroundPlay() {
        String key = mAppContext.getString(R.string.pref_key_enable_background_play);
        return mSharedPreferences.getBoolean(key, false);
    }

    public int getPlayer() {
        String key = mAppContext.getString(R.string.pref_key_player);
        String value = mSharedPreferences.getString(key, "");
        try {
            return Integer.valueOf(value).intValue();
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    public boolean getUsingMediaCodec() {
        String key = mAppContext.getString(R.string.pref_key_using_media_codec);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getUsingMediaCodecAutoRotate() {
        String key = mAppContext.getString(R.string.pref_key_using_media_codec_auto_rotate);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getMediaCodecHandleResolutionChange() {
        String key = mAppContext.getString(R.string.pref_key_media_codec_handle_resolution_change);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getUsingOpenSLES() {
        String key = mAppContext.getString(R.string.pref_key_using_opensl_es);
        return mSharedPreferences.getBoolean(key, false);
    }

    public String getPixelFormat() {
        String key = mAppContext.getString(R.string.pref_key_pixel_format);
        return mSharedPreferences.getString(key, "");
    }

    public boolean getEnableNoView() {
        String key = mAppContext.getString(R.string.pref_key_enable_no_view);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getEnableSurfaceView() {
        String key = mAppContext.getString(R.string.pref_key_enable_surface_view);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getEnableTextureView() {
        String key = mAppContext.getString(R.string.pref_key_enable_texture_view);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getEnableDetachedSurfaceTextureView() {
        String key = mAppContext.getString(R.string.pref_key_enable_detached_surface_texture);
        return mSharedPreferences.getBoolean(key, false);
    }

    public boolean getUsingMediaDataSource() {
        String key = mAppContext.getString(R.string.pref_key_using_mediadatasource);
        return mSharedPreferences.getBoolean(key, false);
    }

    /** 直播低延迟模式：关闭启动缓冲、不限制缓冲上限、nobuffer、缩短探测 */
    public boolean getLiveLowLatency() {
        String key = mAppContext.getString(R.string.pref_key_live_low_latency);
        return mSharedPreferences.getBoolean(key, false);
    }

    /** 以视频为主时钟：画面不等音频输出缓冲，音频跟着视频调整 */
    public boolean getLiveVideoMaster() {
        String key = mAppContext.getString(R.string.pref_key_live_video_master);
        return mSharedPreferences.getBoolean(key, false);
    }

    /** 追帧：缓冲超过 500ms 时 1.2 倍速播放，降到 200ms 以下恢复 */
    public boolean getLiveChase() {
        String key = mAppContext.getString(R.string.pref_key_live_chase);
        return mSharedPreferences.getBoolean(key, false);
    }

    /** 缓冲超过该值（毫秒）时丢掉缓冲直接跳到下一个关键帧，0 表示关闭 */
    public int getLiveDropMs() {
        return getIntString(R.string.pref_key_live_drop_ms, 0);
    }

    /** HUD 延迟超过该值（毫秒）时标红 */
    public int getLatencyTargetMs() {
        return getIntString(R.string.pref_key_latency_target_ms, 300);
    }

    /** 端到端延迟校时用的 NTP 服务器，空表示不校时 */
    public String getNtpServer() {
        String key = mAppContext.getString(R.string.pref_key_ntp_server);
        return mSharedPreferences.getString(key, mAppContext.getString(R.string.pref_default_ntp_server)).trim();
    }

    private int getIntString(int keyId, int defaultValue) {
        String value = mSharedPreferences.getString(mAppContext.getString(keyId), null);
        if (value == null)
            return defaultValue;
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException e) {
            return defaultValue;
        }
    }

    public String getLastDirectory() {
        String key = mAppContext.getString(R.string.pref_key_last_directory);
        return mSharedPreferences.getString(key, "/");
    }

    public void setLastDirectory(String path) {
        String key = mAppContext.getString(R.string.pref_key_last_directory);
        mSharedPreferences.edit().putString(key, path).apply();
    }
}
