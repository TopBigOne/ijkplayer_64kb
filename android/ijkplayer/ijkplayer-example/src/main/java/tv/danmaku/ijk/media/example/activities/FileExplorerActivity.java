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

package tv.danmaku.ijk.media.example.activities;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;

import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentTransaction;
import android.text.TextUtils;

import com.squareup.otto.Subscribe;

import java.io.File;
import java.io.IOException;

import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.example.application.AppActivity;
import tv.danmaku.ijk.media.example.application.Settings;
import tv.danmaku.ijk.media.example.eventbus.FileExplorerEvents;
import tv.danmaku.ijk.media.example.fragments.FileListFragment;

/** 本地文件浏览：从 Sample 首页的「本地视频」分类进入，选中文件后播放。 */
public class FileExplorerActivity extends AppActivity {
    private static final int REQUEST_READ_STORAGE = 1;

    private Settings mSettings;

    public static void intentTo(Context context) {
        context.startActivity(new Intent(context, FileExplorerActivity.class));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (mSettings == null) {
            mSettings = new Settings(this);
        }

        requestStoragePermissionIfNeeded();

        openInitialDirectory();
    }

    /**
     * 优先打开上次浏览的目录，否则打开外部存储根目录。
     * 「上次目录」默认值是 "/"，新系统上根目录不可读，所以要求目录能列出内容才使用。
     */
    private void openInitialDirectory() {
        String lastDirectory = mSettings.getLastDirectory();
        if (!TextUtils.isEmpty(lastDirectory) && !"/".equals(lastDirectory)
                && new File(lastDirectory).listFiles() != null)
            doOpenDirectory(lastDirectory, false);
        else
            doOpenDirectory(Environment.getExternalStorageDirectory().getAbsolutePath(), false);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQUEST_READ_STORAGE)
            return;
        // 授权前列表是空的，任一权限授予后重新加载
        for (int result : grantResults) {
            if (result == PackageManager.PERMISSION_GRANTED) {
                openInitialDirectory();
                return;
            }
        }
    }

    /** 只有浏览本地文件需要读存储权限，因此在这里申请，而不是 App 一启动就弹。 */
    private void requestStoragePermissionIfNeeded() {
        String[] permissions = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                ? new String[]{Manifest.permission.READ_MEDIA_VIDEO, Manifest.permission.READ_MEDIA_AUDIO}
                : new String[]{Manifest.permission.READ_EXTERNAL_STORAGE};
        for (String permission : permissions) {
            if (ContextCompat.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this, permissions, REQUEST_READ_STORAGE);
                return;
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();

        FileExplorerEvents.getBus().register(this);
    }

    @Override
    protected void onPause() {
        super.onPause();

        FileExplorerEvents.getBus().unregister(this);
    }

    private void doOpenDirectory(String path, boolean addToBackStack) {
        Fragment newFragment = FileListFragment.newInstance(path);
        FragmentTransaction transaction = getSupportFragmentManager().beginTransaction();

        transaction.replace(R.id.body, newFragment);

        if (addToBackStack)
            transaction.addToBackStack(null);
        transaction.commit();
    }

    @Subscribe
    public void onClickFile(FileExplorerEvents.OnClickFile event) {
        File f = event.mFile;
        try {
            f = f.getAbsoluteFile();
            f = f.getCanonicalFile();
            if (TextUtils.isEmpty(f.toString()))
                f = new File("/");
        } catch (IOException e) {
            e.printStackTrace();
        }

        if (f.isDirectory()) {
            String path = f.toString();
            mSettings.setLastDirectory(path);
            doOpenDirectory(path, true);
        } else if (f.exists()) {
            VideoActivity.intentTo(this, f.getPath(), f.getName());
        }
    }
}
