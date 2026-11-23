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

import android.os.Bundle;

import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.os.LocaleListCompat;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceFragmentCompat;

import tv.danmaku.ijk.media.example.R;

public class SettingsFragment extends PreferenceFragmentCompat {
    public static SettingsFragment newInstance() {
        SettingsFragment f = new SettingsFragment();
        return f;
    }

    @Override
    public void onCreatePreferences(Bundle bundle, String s) {
        addPreferencesFromResource(R.xml.settings);
        setupLanguagePreference();
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
