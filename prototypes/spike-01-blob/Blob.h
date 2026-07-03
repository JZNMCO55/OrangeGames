// spike-01-blob —— 流体史莱姆 context-sensitive 手感 Spike 1 · Loop B
//
// 软体 blob 仿真（**纯游戏代码，不是引擎 feature**——Box2D 不做软体）。
// 设计规格：../spike-01-blob-feel.md（"核心架构：果冻感和精确落点不在同一层"
// + "循环 B：先 baseline 后加档" + "技术方案：自写 Verlet/PBD ~12-20 质点"）。
//
// 分层（spec 核心洞察）：
//   * gameplay 真相层 = Loop A 的 control point（隐藏刚体，精确落点）；
//   * 视觉表现层 = 本 Blob（可见软体）：用弹簧软软追 control point，随便晃。
// 二者解耦 → "落点 vs 果冻" 从 0/1 取舍变成可调容差区间（= apex 分叉预算）。
//
// 仿真方案：
//   * 质点：一圈 N 个 perimeter 点（默认 14，按角度 CCW）+ 1 个中心点。
//   * 积分：Verlet（pos + prevPos 编码速度），半隐式欧拉形态推进弹簧。
//   * 弹簧吸附（spec 核心）：每点朝 (control point + 该点 rest offset) 施加
//     Hooke 力 + velocity 阻尼（stiffness / damping 走 slider）。
//   * PBD 约束（迭代次数走 slider，spec 地基 #3 sim 先稳）：
//       - distance 约束：相邻 perimeter 边 + center→perimeter 辐条；
//       - area 约束：保 perimeter 多边形面积不塌（被挤压后能弹回 = 钻缝）。
//   * 碰撞：每质点 vs 关卡盒 AABB analytic 投影。引擎已提供 raycast/overlap 查询，
//     但软体每质点连续投影用 analytic 更省更稳（且外观由此调好），故 Loop A 的接地/
//     角落探针迁到引擎查询后，blob 仍保留 analytic；几何直接读单一 mLevel（不再另存
//     一份副本），Step 以模板收任意暴露 .min/.max 的盒类型，与 main.cpp 的 LevelBox 解耦。
//
// 本 Blob 不带独立重力——control point 已承载重力，blob 只负责"软软跟随"，
// 跟随途中的 overshoot / 拖尾 / 分叉就是 juice（spec：纯位移途中随便分叉）。

#ifndef SPIKE01_BLOB_H
#define SPIKE01_BLOB_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace spike01
{

    // ---------------------------------------------------------------------------
    // Blob 仿真旋钮（全部走 ImGui slider；baseline = "明显果冻但不炸"软值，
    // 供用户第二天真机调，详见 spec "循环 B：先 baseline 后加档"）。
    // 本次只把 stiffness/damping 暴露成单档 slider，**不**预先写状态机
    // （状态相关 stiffness = 机制 1，是 Step B，留给用户调参时按需加）。
    // ---------------------------------------------------------------------------
    struct BlobParams
    {
        // —— 弹簧吸附 control point（spec 核心）——
        float stiffness = 200.0f; // Hooke 系数：朝 target 的吸附硬度（越大越跟手越不果冻）
        float damping   = 8.0f;   // 弹簧阻尼：velocity 衰减（< 2*sqrt(k) 欠阻尼 = 弹/晃）

        // —— PBD 约束（spec 地基 #3：sim 先稳——快落/起跳速度范围里不能炸）——
        int   constraintIters = 8;    // 约束迭代次数（越多越稳越接近刚体）
        float edgeStiffness   = 1.0f; // distance 约束（相邻边 + 辐条）强度 0..1
        float areaStiffness   = 0.5f; // area 约束强度 0..1（0=易塌, 1=强保体积）

        // —— blob 形状 ——
        float blobRadius = 0.5f;  // 静止时 perimeter 圈半径（改动 live 重建 rest 几何）
        float skin       = 0.03f; // 质点碰撞皮肤厚度 u（perimeter 线不嵌入平台）

        // —— 渲染开关 ——
        bool drawSpokes   = true; // 画 center→perimeter 辐条
        bool drawCpMarker = true; // 叠画 control point 小标记（对比 gameplay 真相）
    };

    // 碰撞几何以模板参数收：任何暴露 `glm::vec2 min` / `glm::vec2 max` 成员的盒类型都行
    // （main.cpp 的 LevelBox 即是）。如此 Blob 既不依赖 main.cpp 的完整 LevelBox 定义，
    // 又能直接复用单一 mLevel 数据源、免维护第二份 AABB 副本。

    // ===========================================================================
    // Blob —— 自写 Verlet + PBD 软体。所有方法 inline（header-only，仅 main.cpp 包含）。
    // 质点布局：索引 [0 .. mPerimeter-1] = perimeter（CCW），[mPerimeter] = center。
    // ===========================================================================
    class Blob
    {
    public:
        // pointCount = perimeter 质点数（不含中心点）。固定，不做 slider；
        // 默认 14（+1 中心 = 15，落在 spec 的 12-20 区间）。
        explicit Blob(int pointCount = 14)
            : mPerimeter(std::max(3, pointCount))
        {
        }

        // 用 control point 当前位置初始化：perimeter 圈摆在 cp 周围，
        // prevPos = pos（静止起步，不带初速度），重建 rest 几何。
        void Reset(glm::vec2 center, float radius)
        {
            radius          = std::max(0.05f, radius);
            const int total = mPerimeter + 1;
            mPos.assign(total, glm::vec2(0.0f));
            mPrev.assign(total, glm::vec2(0.0f));
            mRestOffset.assign(total, glm::vec2(0.0f));
            mAreaGrad.assign(mPerimeter, glm::vec2(0.0f));

            BuildRestGeometry(radius);
            for (int i = 0; i < total; ++i)
            {
                mPos[i]  = center + mRestOffset[i];
                mPrev[i] = mPos[i];
            }
            mInit = true;
        }

        // 推进一个固定子步（与 control point 同 FixedStep，spec 地基 #2：
        // 变步长会让 blob 易炸，故固定步长）：
        //   1. Verlet 半隐式积分（弹簧吸附 target + 阻尼）；
        //   2. PBD 约束迭代（distance + area + 碰撞）；
        //   3. 末尾再投影一次碰撞确保不嵌入。
        template <class LevelBoxT>
        void Step(float dt, glm::vec2 cpPos,
                  const std::vector<LevelBoxT>& level, const BlobParams& p)
        {
            if (!mInit)
            {
                Reset(cpPos, p.blobRadius);
            }
            // blobRadius slider 改动 → 重建 rest 几何（保留当前 positions，仅改 target/rest）。
            if (std::abs(p.blobRadius - mRadius) > 1e-5f)
            {
                BuildRestGeometry(std::max(0.05f, p.blobRadius));
            }

            const int   total = static_cast<int>(mPos.size());
            const float invDt = (dt > 1e-6f) ? (1.0f / dt) : 0.0f;

            // —— 1. Verlet 半隐式积分：弹簧吸附 (cp + restOffset) + velocity 阻尼 ——
            // a = k*(target - pos) - c*vel；newVel = vel + a*dt；newPos = pos + newVel*dt。
            // 半隐式欧拉对弹簧无条件稳定到 k*dt^2 < 4（60Hz 下 k<14400），slider 上限内安全。
            for (int i = 0; i < total; ++i)
            {
                const glm::vec2 target = cpPos + mRestOffset[i];
                const glm::vec2 vel    = (mPos[i] - mPrev[i]) * invDt;
                const glm::vec2 accel  = p.stiffness * (target - mPos[i]) - p.damping * vel;
                const glm::vec2 newVel = vel + accel * dt;
                const glm::vec2 newPos = mPos[i] + newVel * dt;
                mPrev[i]               = mPos[i];
                mPos[i]                = newPos;
            }

            // —— 2. PBD 约束迭代（迭代次数走 slider）——
            const int iters = std::max(1, p.constraintIters);
            for (int it = 0; it < iters; ++it)
            {
                SatisfyDistance(p.edgeStiffness);
                SatisfyArea(p.areaStiffness);
                SatisfyCollision(level, p.skin);
            }
            // —— 3. 末尾碰撞投影（约束可能把点又推回盒内，最后再清一次）——
            SatisfyCollision(level, p.skin);
        }

        // 视觉质心 = perimeter 点平均（用于 apex 分叉测量；不含中心点更贴"看起来的中心"）。
        glm::vec2 Centroid() const
        {
            if (mPerimeter <= 0 || static_cast<int>(mPos.size()) < mPerimeter)
            {
                return glm::vec2(0.0f); // 未 init（mPos 为空）时安全返回
            }
            glm::vec2 sum(0.0f);
            for (int i = 0; i < mPerimeter; ++i)
            {
                sum += mPos[i];
            }
            return sum / static_cast<float>(mPerimeter);
        }

        int PerimeterCount() const noexcept
        {
            return mPerimeter;
        }
        int CenterIndex() const noexcept
        {
            return mPerimeter;
        }
        const std::vector<glm::vec2>& Positions() const noexcept
        {
            return mPos;
        }
        bool Initialized() const noexcept
        {
            return mInit;
        }

    private:
        // 重建 rest 几何（restOffset / 边 rest 长 / 辐条 rest 长 / rest 面积）。
        // CCW 排列保证 signed area 为正，area 约束梯度方向自洽。
        void BuildRestGeometry(float radius)
        {
            mRadius            = radius;
            const float kTwoPi = 6.28318530718f;
            for (int i = 0; i < mPerimeter; ++i)
            {
                const float ang = kTwoPi * static_cast<float>(i) / static_cast<float>(mPerimeter);
                mRestOffset[i]  = glm::vec2(std::cos(ang), std::sin(ang)) * radius;
            }
            mRestOffset[mPerimeter] = glm::vec2(0.0f); // 中心点

            // 相邻 perimeter 边 rest 长（正多边形等长，仍逐边存以备非规则形状）。
            mEdgeRest.assign(mPerimeter, 0.0f);
            for (int i = 0; i < mPerimeter; ++i)
            {
                const int j  = (i + 1) % mPerimeter;
                mEdgeRest[i] = glm::length(mRestOffset[j] - mRestOffset[i]);
            }
            mSpokeRest = radius; // center→perimeter 辐条 rest 长

            // rest 面积（perimeter 多边形 signed area）。
            mRestArea = PolygonSignedArea(mRestOffset);
        }

        // PBD distance 约束：相邻 perimeter 边 + center→perimeter 辐条。等质量，两端各动一半。
        void SatisfyDistance(float stiffness)
        {
            if (stiffness <= 0.0f)
            {
                return;
            }
            for (int i = 0; i < mPerimeter; ++i)
            {
                SolveDistance(i, (i + 1) % mPerimeter, mEdgeRest[i], stiffness);
            }
            const int c = mPerimeter;
            for (int i = 0; i < mPerimeter; ++i)
            {
                SolveDistance(c, i, mSpokeRest, stiffness);
            }
        }

        void SolveDistance(int a, int b, float rest, float stiffness)
        {
            glm::vec2   d   = mPos[b] - mPos[a];
            const float len = glm::length(d);
            if (len < 1e-6f)
            {
                return;
            }
            const float     diff = (len - rest) / len;
            const glm::vec2 corr = d * (0.5f * stiffness * diff);
            mPos[a] += corr;
            mPos[b] -= corr;
        }

        // PBD area 约束：把 perimeter 多边形面积拉回 mRestArea（保体积，被挤压后弹回）。
        // C = area - restArea；Δp_i = -(C / Σ|∇_j C|^2) * ∇_i C（等质量），乘 stiffness。
        void SatisfyArea(float stiffness)
        {
            if (stiffness <= 0.0f || mPerimeter < 3)
            {
                return;
            }
            const int   N    = mPerimeter;
            const float area = PolygonSignedArea(mPos, N);
            const float C    = area - mRestArea;

            float sumSq = 0.0f;
            for (int i = 0; i < N; ++i)
            {
                const int ip = (i + 1) % N;
                const int im = (i - 1 + N) % N;
                // ∇_i(signed area) = 0.5 * (y_{i+1} - y_{i-1}, x_{i-1} - x_{i+1})
                mAreaGrad[i] = 0.5f * glm::vec2(mPos[ip].y - mPos[im].y,
                                                mPos[im].x - mPos[ip].x);
                sumSq += glm::dot(mAreaGrad[i], mAreaGrad[i]);
            }
            if (sumSq < 1e-9f)
            {
                return;
            }
            const float lambda = C / sumSq;
            for (int i = 0; i < N; ++i)
            {
                mPos[i] -= stiffness * lambda * mAreaGrad[i];
            }
        }

        // 每质点 vs 关卡盒 AABB（skin 膨胀）投影出盒。出盒方向按入射面（质点上一子步
        // mPrev 在盒外哪侧），不用"最浅穿透面"：地面盒薄，最浅面会把落地下陷的底部
        // 质点从底面挤穿、困在地面下（陷地根因）；入射面恒把上方来的点弹回顶面。
        template <class LevelBoxT>
        void SatisfyCollision(const std::vector<LevelBoxT>& level, float skin)
        {
            const int total = static_cast<int>(mPos.size());
            for (int i = 0; i < total; ++i)
            {
                glm::vec2&      pos  = mPos[i];
                const glm::vec2 prev = mPrev[i]; // 入射参照（本子步积分前位置）
                for (const auto& b : level)
                {
                    const float minx = b.min.x - skin;
                    const float maxx = b.max.x + skin;
                    const float miny = b.min.y - skin;
                    const float maxy = b.max.y + skin;
                    if (pos.x <= minx || pos.x >= maxx || pos.y <= miny || pos.y >= maxy)
                    {
                        continue; // 不在膨胀盒内
                    }
                    const float dxL = pos.x - minx; // 四面穿透深度
                    const float dxR = maxx - pos.x;
                    const float dyB = pos.y - miny;
                    const float dyT = maxy - pos.y;

                    // 入射面：prev 在盒外哪侧 → 从该侧出盒（每轴至多一侧）。
                    bool  haveX = false, haveY = false;
                    float penX = 0.0f, tgtX = 0.0f;
                    float penY = 0.0f, tgtY = 0.0f;
                    if (prev.x <= minx)
                    {
                        haveX = true;
                        penX  = dxL;
                        tgtX  = minx;
                    }
                    else if (prev.x >= maxx)
                    {
                        haveX = true;
                        penX  = dxR;
                        tgtX  = maxx;
                    }
                    if (prev.y <= miny)
                    {
                        haveY = true;
                        penY  = dyB;
                        tgtY  = miny;
                    }
                    else if (prev.y >= maxy)
                    {
                        haveY = true;
                        penY  = dyT;
                        tgtY  = maxy;
                    }

                    if (haveX && (!haveY || penX <= penY))
                    {
                        pos.x = tgtX;
                    }
                    else if (haveY)
                    {
                        pos.y = tgtY;
                    }
                    else
                    {
                        // prev 也在盒内（无入射面，罕见/首帧）：退回最浅面兜底。
                        const float m = std::min(std::min(dxL, dxR), std::min(dyB, dyT));
                        if (m == dxL)
                            pos.x = minx;
                        else if (m == dxR)
                            pos.x = maxx;
                        else if (m == dyB)
                            pos.y = miny;
                        else
                            pos.y = maxy;
                    }
                }
            }
        }

        static float PolygonSignedArea(const std::vector<glm::vec2>& pts, int count)
        {
            float area = 0.0f;
            for (int i = 0; i < count; ++i)
            {
                const int j = (i + 1) % count;
                area += pts[i].x * pts[j].y - pts[j].x * pts[i].y;
            }
            return 0.5f * area;
        }
        // 重载：用 restOffset（含末尾中心点）算 perimeter 面积——只取前 mPerimeter 个。
        float PolygonSignedArea(const std::vector<glm::vec2>& pts) const
        {
            return PolygonSignedArea(pts, mPerimeter);
        }

        int mPerimeter; // perimeter 点数（不含中心点）

        std::vector<glm::vec2> mPos;        // 质点当前位置（[0..N-1]=perimeter, [N]=center）
        std::vector<glm::vec2> mPrev;       // 上一子步位置（Verlet 速度 = (pos-prev)/dt）
        std::vector<glm::vec2> mRestOffset; // 相对 cp 的静止偏移（spring target = cp + restOffset）
        std::vector<glm::vec2> mAreaGrad;   // area 约束梯度 scratch（size = mPerimeter）

        std::vector<float> mEdgeRest;          // 相邻 perimeter 边 rest 长（size = mPerimeter）
        float              mSpokeRest = 0.0f;  // 辐条 rest 长（= radius）
        float              mRestArea  = 0.0f;  // perimeter 多边形 rest 面积
        float              mRadius    = -1.0f; // 当前 rest 半径（!= slider 则重建）
        bool               mInit      = false;
    };

} // namespace spike01

#endif // SPIKE01_BLOB_H
