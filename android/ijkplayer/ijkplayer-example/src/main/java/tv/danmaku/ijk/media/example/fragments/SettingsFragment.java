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

package tv.danmaku.ijk.media.example.fragments;

import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.os.LocaleListCompat;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceFragmentCompat;

import tv.danmaku.ijk.media.example.BuildConfig;
import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.player.IjkMediaPlayer;

public class SettingsFragment extends PreferenceFragmentCompat {
    public static SettingsFragment newInstance() {
        SettingsFragment f = new SettingsFragment();
        return f;
    }

    @Override
    public void onCreatePreferences(Bundle bundle, String s) {
        addPreferencesFromResource(R.xml.settings);
        setupLanguagePreference();
        setupThirdPartyLibsPreference();
    }

    /**
     * 「第三方库」：Native 部分在运行时从 libijkplayer.so 读取实际链接的版本，
     * Java 部分来自 build.gradle 中依赖列表生成的 BuildConfig.THIRD_PARTY_JAVA_LIBS。
     */
    private void setupThirdPartyLibsPreference() {
        Preference pref = findPreference(getString(R.string.pref_key_third_party_libs));
        if (pref == null)
            return;

        pref.setOnPreferenceClickListener(new Preference.OnPreferenceClickListener() {
            @Override
            public boolean onPreferenceClick(Preference preference) {
                showThirdPartyLibsDialog();
                return true;
            }
        });
    }

    private void showThirdPartyLibsDialog() {
        LinearLayout list = new LinearLayout(requireContext());
        list.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        list.setPadding(pad, dp(8), pad, 0);

        addSectionTitle(list, getString(R.string.third_party_libs_native), false);
        String nativeVersions;
        try {
            nativeVersions = IjkMediaPlayer.getNativeLibraryVersions();
        } catch (Throwable e) {
            nativeVersions = "?\t" + e;
        }
        for (String line : nativeVersions.split("\n")) {
            if (line.isEmpty())
                continue;
            String[] kv = line.split("\t", 2);
            addLibItem(list, kv[0], kv.length > 1 ? kv[1] : "");
        }

        addSectionTitle(list, getString(R.string.third_party_libs_java), true);
        for (String lib : BuildConfig.THIRD_PARTY_JAVA_LIBS.split("\n")) {
            // group:artifact:version → 名称 artifact，版本 version
            String[] gav = lib.split(":");
            if (gav.length == 3)
                addLibItem(list, gav[1], gav[2]);
            else
                addLibItem(list, lib, "");
        }

        ScrollView scroll = new ScrollView(requireContext());
        scroll.addView(list);

        new AlertDialog.Builder(requireContext())
                .setTitle(R.string.pref_title_third_party_libs)
                .setView(scroll)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    private void addSectionTitle(LinearLayout parent, String title, boolean withTopGap) {
        TextView tv = new TextView(requireContext());
        tv.setText(title);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        tv.setPadding(0, withTopGap ? dp(16) : 0, 0, dp(4));
        parent.addView(tv);
    }

    /** 库名一行；版本另起一行并缩进，版本过长换行时续行仍保持缩进 */
    private void addLibItem(LinearLayout parent, String name, String version) {
        TextView nameView = new TextView(requireContext());
        nameView.setText(name);
        nameView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        nameView.setPadding(0, dp(6), 0, 0);
        parent.addView(nameView);

        TextView versionView = new TextView(requireContext());
        versionView.setText(version);
        versionView.setTextIsSelectable(true);
        versionView.setTypeface(Typeface.MONOSPACE);
        versionView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        versionView.setAlpha(0.7f);
        versionView.setPadding(dp(16), 0, 0, 0);
        parent.addView(versionView);
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    /**
     * 应用语言：值为 "" / "zh" / "en"，"" 表示跟随系统。
     * 通过 AppCompatDelegate 设置，Android 13+ 与系统「应用语言」同步，低版本由 AppCompat 自动保存；
     * 设置后 AppCompat 会自动重建当前 Activity。
     */
    private void setupLanguagePreference() {
        ListPreference pref = findPreference(getString(R.string.pref_key_language));
        if (pref == null)
            return;

        LocaleListCompat current = AppCompatDelegate.getApplicationLocales();
        pref.setValue(current.isEmpty() ? "" : current.get(0).getLanguage());

        pref.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
            @Override
            public boolean onPreferenceChange(Preference preference, Object newValue) {
                String tag = (String) newValue;
                AppCompatDelegate.setApplicationLocales(tag.isEmpty()
                        ? LocaleListCompat.getEmptyLocaleList()
                        : LocaleListCompat.forLanguageTags(tag));
                return true;
            }
        });
    }
}
