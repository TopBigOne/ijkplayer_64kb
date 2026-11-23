package tv.danmaku.ijk.media.example.fragments;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.text.InputType;
import android.text.TextUtils;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.content.ContextCompat;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.List;

import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.example.activities.VideoActivity;
import tv.danmaku.ijk.media.example.content.SampleMediaCatalog;

/** 某一分类下的测试链接列表；分类允许时顶部有「自定义地址」入口。 */
public class SampleMediaListFragment extends Fragment {
    private static final String ARG_CATEGORY = "category";
    private static final String PREFS = "sample_media";
    private static final String KEY_CUSTOM_URL_PREFIX = "custom_url_";

    /** url 为 null 表示「自定义地址」入口。 */
    private static final class Row {
        final String name;
        final String url;

        Row(String name, String url) {
            this.name = name;
            this.url = url;
        }

        boolean isCustom() {
            return url == null;
        }
    }

    public static SampleMediaListFragment newInstance(int categoryIndex) {
        SampleMediaListFragment f = new SampleMediaListFragment();
        Bundle args = new Bundle();
        args.putInt(ARG_CATEGORY, categoryIndex);
        f.setArguments(args);
        return f;
    }

    private SampleMediaCatalog.Category mCategory;
    private RowAdapter mAdapter;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        RecyclerView recyclerView = (RecyclerView) inflater.inflate(R.layout.fragment_sample_list, container, false);
        recyclerView.setLayoutManager(new LinearLayoutManager(inflater.getContext()));

        int index = getArguments() != null ? getArguments().getInt(ARG_CATEGORY, -1) : -1;
        mCategory = SampleMediaCatalog.category(index);
        if (mCategory == null)
            return recyclerView;

        List<Row> rows = new ArrayList<>();
        if (mCategory.allowCustomUrl)
            rows.add(new Row("输入自定义地址", null));
        for (SampleMediaCatalog.Item item : mCategory.items)
            rows.add(new Row(item.name, item.url));

        mAdapter = new RowAdapter(rows);
        recyclerView.setAdapter(mAdapter);
        return recyclerView;
    }

    private SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** 自定义地址按分类分别记住上次输入。 */
    private String savedCustomUrl(Context context) {
        return prefs(context).getString(KEY_CUSTOM_URL_PREFIX + mCategory.title, null);
    }

    private void onRowClicked(Context context, Row row) {
        if (row.isCustom())
            showCustomUrlDialog(context);
        else
            VideoActivity.intentTo(context, row.url, row.name);
    }

    private void showCustomUrlDialog(final Context context) {
        final EditText input = new EditText(context);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        input.setSingleLine(true);
        input.setHint(mCategory.customUrlHint);
        String saved = savedCustomUrl(context);
        if (!TextUtils.isEmpty(saved)) {
            input.setText(saved);
            input.setSelection(saved.length());
        }

        new AlertDialog.Builder(context)
                .setTitle("输入播放地址")
                .setView(input)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton("播放", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        String url = input.getText().toString().trim();
                        if (url.isEmpty())
                            return;
                        prefs(context).edit().putString(KEY_CUSTOM_URL_PREFIX + mCategory.title, url).apply();
                        if (mAdapter != null)
                            mAdapter.notifyItemChanged(0);  // 自定义入口固定在第一行
                        VideoActivity.intentTo(context, url, "自定义地址");
                    }
                })
                .show();
    }

    private final class RowAdapter extends RecyclerView.Adapter<RowAdapter.Holder> {
        private final List<Row> mRows;

        RowAdapter(List<Row> rows) {
            mRows = rows;
        }

        @NonNull
        @Override
        public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_sample_media, parent, false);
            return new Holder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull Holder holder, int position) {
            holder.bind(mRows.get(position));
        }

        @Override
        public int getItemCount() {
            return mRows.size();
        }

        final class Holder extends RecyclerView.ViewHolder {
            final View accent;
            final TextView name;
            final TextView url;
            final TextView action;

            Holder(@NonNull View itemView) {
                super(itemView);
                accent = itemView.findViewById(R.id.accent);
                name = itemView.findViewById(R.id.name);
                url = itemView.findViewById(R.id.url);
                action = itemView.findViewById(R.id.action);
                itemView.findViewById(R.id.card).setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        int pos = getBindingAdapterPosition();
                        if (pos != RecyclerView.NO_POSITION)
                            onRowClicked(v.getContext(), mRows.get(pos));
                    }
                });
            }

            void bind(Row row) {
                Context context = itemView.getContext();
                float density = context.getResources().getDisplayMetrics().density;
                int color = mCategory.color;
                boolean custom = row.isCustom();

                accent.setBackgroundColor(custom ? color : SampleCategoryListFragment.withAlpha(color, 0x66));
                name.setText(row.name);
                name.setTextColor(custom ? color : ContextCompat.getColor(context, R.color.sample_text_primary));

                String saved = custom ? savedCustomUrl(context) : null;
                url.setText(custom ? (TextUtils.isEmpty(saved) ? mCategory.customUrlHint : saved) : row.url);

                action.setText(custom ? "✎" : "▶");
                action.setTextColor(color);
                action.setBackground(SampleCategoryListFragment.roundRect(
                        SampleCategoryListFragment.withAlpha(color, 0x1F), 18 * density));
            }
        }
    }
}
