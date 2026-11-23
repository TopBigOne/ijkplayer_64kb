package tv.danmaku.ijk.media.example.fragments;

import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.util.List;

import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.example.activities.FileExplorerActivity;
import tv.danmaku.ijk.media.example.activities.SampleMediaActivity;
import tv.danmaku.ijk.media.example.content.SampleMediaCatalog;

/** Sample 首页：以卡片展示测试链接分类，点击进入该分类的链接列表。 */
public class SampleCategoryListFragment extends Fragment {

    public static SampleCategoryListFragment newInstance() {
        return new SampleCategoryListFragment();
    }

    /** 圆角纯色背景。 */
    static GradientDrawable roundRect(int color, float radiusPx) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(radiusPx);
        return d;
    }

    static int withAlpha(int color, int alpha) {
        return (color & 0x00FFFFFF) | (alpha << 24);
    }

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
        RecyclerView recyclerView = (RecyclerView) inflater.inflate(R.layout.fragment_sample_list, container, false);
        recyclerView.setLayoutManager(new LinearLayoutManager(inflater.getContext()));
        recyclerView.setAdapter(new CategoryAdapter(SampleMediaCatalog.categories()));
        return recyclerView;
    }

    private static final class CategoryAdapter extends RecyclerView.Adapter<CategoryAdapter.Holder> {
        private final List<SampleMediaCatalog.Category> mCategories;

        CategoryAdapter(List<SampleMediaCatalog.Category> categories) {
            mCategories = categories;
        }

        @NonNull
        @Override
        public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_sample_category, parent, false);
            return new Holder(view);
        }

        @Override
        public void onBindViewHolder(@NonNull Holder holder, int position) {
            holder.bind(mCategories.get(position));
        }

        @Override
        public int getItemCount() {
            return mCategories.size();
        }

        final class Holder extends RecyclerView.ViewHolder {
            final TextView badge;
            final TextView title;
            final TextView count;
            final TextView description;

            Holder(@NonNull View itemView) {
                super(itemView);
                badge = itemView.findViewById(R.id.badge);
                title = itemView.findViewById(R.id.title);
                count = itemView.findViewById(R.id.count);
                description = itemView.findViewById(R.id.description);
                itemView.findViewById(R.id.card).setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        int pos = getBindingAdapterPosition();
                        if (pos == RecyclerView.NO_POSITION)
                            return;
                        if (mCategories.get(pos).opensFileBrowser)
                            FileExplorerActivity.intentTo(v.getContext());
                        else
                            SampleMediaActivity.intentTo(v.getContext(), pos);
                    }
                });
            }

            void bind(SampleMediaCatalog.Category c) {
                float density = itemView.getResources().getDisplayMetrics().density;
                badge.setText(c.badge);
                badge.setBackground(roundRect(c.color, 12 * density));
                title.setText(c.title);
                description.setText(c.description);
                count.setVisibility(c.opensFileBrowser ? View.GONE : View.VISIBLE);
                count.setText(String.valueOf(c.items.size()));
                count.setTextColor(c.color);
                count.setBackground(roundRect(withAlpha(c.color, 0x1F), 8 * density));
            }
        }
    }
}
