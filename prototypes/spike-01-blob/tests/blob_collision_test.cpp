// 软体 Blob 碰撞回归测试：blob 落地/硬着陆后须停在地面之上，不陷穿薄地面盒。
// 背景见 Blob::SatisfyCollision——"最浅穿透面"出盒对薄盒会把下陷质点挤穿底面。
// 场景 B(硬砸)复现旧 bug、守住修复；A(正常着陆)/C(静止)防回归。
// ctest -R spike-01-blob.blob_collision

#include "../Blob.h"

#include <cstdio>
#include <limits>
#include <vector>

namespace
{
    // 复刻 main.cpp 的 LevelBox 关键字段（Blob::Step 只读 .min/.max）。
    struct LevelBox
    {
        glm::vec2 min;
        glm::vec2 max;
    };

    // 复刻 main.cpp::MakeLevel()：平地 + 左右平台 + 窄缝两壁（同一份关卡几何）。
    std::vector<LevelBox> MakeLevel()
    {
        return {
            {{-9.0f, -3.5f}, {9.0f, -3.0f}},  // 平地 top y=-3.0，厚 0.5
            {{-6.0f, -0.7f}, {-2.5f, -0.5f}}, // 左平台
            {{2.5f, 0.3f}, {6.0f, 0.5f}},     // 右平台
            {{-0.5f, -3.0f}, {-0.4f, 1.0f}},  // 窄缝左壁
            {{0.4f, -3.0f}, {0.5f, 1.0f}},    // 窄缝右壁
        };
    }

    constexpr float kFixedDt   = 1.0f / 60.0f;
    constexpr float kGroundTop = -3.0f;
} // namespace

int main()
{
    using spike01::Blob;
    using spike01::BlobParams;

    const auto level = MakeLevel();
    BlobParams params; // 全默认（stiffness 200 / damping 8 / iters 8 / radius 0.5 / skin 0.03）

    // 落点选纯地面处（x=8，右平台 x≤6 之外、无窄缝墙），避免砸到平台混淆判据。
    const float     px = 8.0f;
    const glm::vec2 cpRest{px, kGroundTop + 0.28f}; // 脚底贴地面 top → cp 中心 -2.72

    int  failures = 0;
    auto check    = [&](const char* tag, Blob& blob)
    {
        const auto& bp     = blob.Positions();
        const int   n      = blob.PerimeterCount();
        float       lowest = std::numeric_limits<float>::max();
        for (int i = 0; i < n; ++i)
            lowest = std::min(lowest, bp[i].y);
        const glm::vec2 ctr = blob.Centroid();
        std::printf("[%s] 最低质点 y=%.4f | 质心 y=%.4f (地面 top=%.2f, box 底=-3.50)\n",
                    tag, lowest, ctr.y, kGroundTop);
        // 判据：最低点不得穿到地面 top 以下超过 skin + 余量；质心须明显高于地面 top。
        if (lowest < kGroundTop - params.skin - 0.02f)
        {
            std::printf("  FAIL: 最低质点陷入地面下方\n");
            ++failures;
        }
        if (ctr.y < kGroundTop)
        {
            std::printf("  FAIL: 质心陷到地面 top 以下\n");
            ++failures;
        }
    };

    // A 真实着陆：cp 以 ~maxFallSpeed 落到静止位保持，blob 弹簧跟随冲进地面盒。
    {
        Blob      blob(14);
        glm::vec2 cp{px, -1.0f};
        blob.Reset(cp, params.blobRadius);
        const float fall = 25.0f; // maxFallSpeed 量级
        for (int step = 0; step < 600; ++step)
        {
            cp.y = std::max(cpRest.y, cp.y - fall * kFixedDt);
            blob.Step(kFixedDt, cp, level, params);
        }
        check("A 真实着陆", blob);
    }

    // B 硬砸（回归守门）：blob 高空 reset、cp 已在地面 → 大位移猛砸过中线。旧码此处陷到 y≈-4。
    {
        Blob blob(14);
        blob.Reset(glm::vec2{px, 1.0f}, params.blobRadius);
        for (int step = 0; step < 600; ++step)
        {
            blob.Step(kFixedDt, cpRest, level, params);
        }
        check("B 硬砸冲击", blob);
    }

    // C 静止骑线：blob 在 cp 静止位 reset（底部质点 rest≈-3.21 骑中线），验稳态不下陷也不顶飞。
    {
        Blob blob(14);
        blob.Reset(cpRest, params.blobRadius);
        for (int step = 0; step < 600; ++step)
        {
            blob.Step(kFixedDt, cpRest, level, params);
        }
        check("C 静止骑线", blob);
    }

    // D target shape（D0 形变地基）：设 diag(0.7,1.5) 横缩纵拉，空中静止 step，验 perimeter
    // 排成竖椭圆(y范围>x范围) + area/distance 约束保持变换后形状(纵横比不被 PBD 拽回圆)。
    {
        Blob            blob(14);
        const glm::vec2 cp{0.0f, 5.0f}; // 空中，远离关卡碰撞
        blob.Reset(cp, params.blobRadius);
        blob.SetTargetShape(glm::mat2(0.7f, 0.0f, 0.0f, 1.5f)); // 列主序 diag(0.7,1.5)
        const std::vector<LevelBox> empty;
        for (int step = 0; step < 300; ++step)
        {
            blob.Step(kFixedDt, cp, empty, params);
        }
        const auto& bp = blob.Positions();
        const int   n  = blob.PerimeterCount();
        float       mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
        for (int i = 0; i < n; ++i)
        {
            mnx = std::min(mnx, bp[i].x);
            mxx = std::max(mxx, bp[i].x);
            mny = std::min(mny, bp[i].y);
            mxy = std::max(mxy, bp[i].y);
        }
        const float wx = mxx - mnx, wy = mxy - mny;
        std::printf("[D target shape] x范围=%.3f y范围=%.3f 纵横比=%.2f (期望≈2.14 竖椭圆)\n",
                    wx, wy, (wx > 1e-6f) ? wy / wx : 0.0f);
        if (wy <= wx)
        {
            std::printf("  FAIL: 未成竖椭圆\n");
            ++failures;
        }
        if (wx > 1e-6f && wy / wx < 1.6f)
        {
            std::printf("  FAIL: 纵横比不足（target shape 被 PBD 拽回圆?）\n");
            ++failures;
        }
    }

    std::printf("\n结果：%s（failures=%d）\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
