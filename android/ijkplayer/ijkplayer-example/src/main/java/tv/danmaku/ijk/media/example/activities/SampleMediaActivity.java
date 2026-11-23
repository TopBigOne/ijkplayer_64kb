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

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentTransaction;
import android.view.Menu;
import android.view.MenuItem;

import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.example.application.AppActivity;
import tv.danmaku.ijk.media.example.content.SampleMediaCatalog;
import tv.danmaku.ijk.media.example.fragments.SampleCategoryListFragment;
import tv.danmaku.ijk.media.example.fragments.SampleMediaListFragment;

/** 不带分类参数时显示分类目录，带分类参数时显示该分类的测试链接。 */
public class SampleMediaActivity extends AppActivity  {
    private static final String EXTRA_CATEGORY = "category";

    public static Intent newIntent(Context context) {
        return new Intent(context, SampleMediaActivity.class);
    }

    /** 回到 Sample 首页：已在栈中时直接回退到它，不重复创建。 */
    public static void intentTo(Context context) {
        Intent intent = newIntent(context);
        intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        context.startActivity(intent);
    }

    public static void intentTo(Context context, int categoryIndex) {
        Intent intent = newIntent(context);
        intent.putExtra(EXTRA_CATEGORY, categoryIndex);
        context.startActivity(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int categoryIndex = getIntent().getIntExtra(EXTRA_CATEGORY, -1);
        SampleMediaCatalog.Category category = SampleMediaCatalog.category(categoryIndex);
        if (category != null)
            setTitle(category.titleRes);

        if (savedInstanceState != null)
            return;  // 旋转等重建时 FragmentManager 会自动恢复

        Fragment newFragment = category != null
                ? SampleMediaListFragment.newInstance(categoryIndex)
                : SampleCategoryListFragment.newInstance();
        FragmentTransaction transaction = getSupportFragmentManager().beginTransaction();

        transaction.replace(R.id.body, newFragment);
        transaction.commit();
    }

    @Override
    public boolean onPrepareOptionsMenu(Menu menu) {
        boolean show = super.onPrepareOptionsMenu(menu);
        if (!show)
            return show;

        MenuItem item = menu.findItem(R.id.action_sample);
        if (item != null)
            item.setVisible(false);

        return true;
    }
}
