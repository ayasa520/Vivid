# Scene 结构迁移：一层一个对象，行为保持 Vivid

目标：代码结构和数据结构收成「`scene.json` 每个 object 对应一个 `SceneObject`，绘制阶段是同一对象上的 phase」，
删掉把同一层拆成多种 `SceneNode` 身份的做法。
不改已经能用的绘制代数。画面以最后一个能用的 Vivid renderer 为准。

| 快照 | 提交 | 角色 |
|---|---|---|
| 能用的 Vivid | `wallpaper-scene-renderer` `bd0f775535162d520bda12e9d8ad67ddeb27db27` | 行为 / 画面基线 |
| 当前 | `bd0f775` 之上的 identity 线（`f58a863` … `091a6e1` + 本增量） | 薄 `SceneObject`：一层一枚身份袋。绘制代数未动。旧 node 种类还在，但已不再当第二套 authored id |

`bd0f775` 没有 `SceneObject`。一层图在图里是 `SceneNode`，有 effect 时再拆成
WorldNode（场景空间）+ source（私有相机）+ `FinalNode`（发布到 Default）。
本线只加身份袋，没有 `14ef8c1` 那套 dest-STACK / `DestDrawPhase`。
WorldNode / source / FinalNode / effect pass 仍作为 draw handle 存在。
`nodeOwners` **已整个退役**（`8e4c6cc`）：所有注册节点的 `ID()` 本就等于注册值
（灯光节点补齐了赋值），helper 显式 0 的注册对象 ID 默认即 0——回指信息由 node 自持。
解析合同只有一个所有者：`Scene::LayerIdForNode`（现在就是读 `ID()`；
`NodeLayerId` / `ResolveLayerIdForRuntimeNode` / `FindOwningLayerId` 全部委托给它）。

---

## 1. 目标结构

`scene.json` 每个 object 对应一个 `SceneObject`。
parent / attach / visible / origin / dest / effect 数都在这个对象上。
leftover、leftover-MVP、POSTFX、last-pass 是**同一对象**上的绘制阶段，
不是场景图里的兄弟姐妹。

Vulkan 仍然是：编译 render-graph → 上传 → 按 `m_passes` 执行。
这点保持现状，只改**谁拥有身份、谁只是 pass 句柄**。

```
Scene
  sceneObjects          authored 身份（按层 id）
  dest stack            2D 放置
  sceneGraph            只挂「要进图」的 draw handle，不再当第二套 object 树

SceneObject             一层一枚
  origin / scale / angles
  parent / attach
  FetchDest
  leftover / image_490 / last-pass / postfx mesh
  DestDrawPhase 标签    给已有 pass 用，不为此再 new SceneNode

SceneNode               draw handle：mesh、material、text、camera 名、输出 RT
                        不再复制一层 origin/parent/id
```

一句话：`SceneObject` 是层；`SceneNode` 是「怎么画出这一刀」。
不要再为 leftover-MVP / FinalComposite / source 各做一枚带同一 `id` 的 `SceneNode`。

---

## 2. 建议迁过去（值得做）

这些迁的是**所有权**，不是改 `g_MVP` 怎么乘。

### 2.1 加深 `SceneObject`，把它当成唯一 authored 身份

本线已经有、继续当主结构（`SceneObject.h`，`f58a863`）：

- `SceneObjectKind`、`id`、`name`
- `origin` / `scale` / `angles`（parse 快照；运行时放置仍写 node，见 §5.2 余项）
- `local_visible`、`parent_id` + `attachment`
- `effect_count`、`passthrough`
- image runtime state（size + alignment，原 `imageLayers` 的内容）
- `Scene::sceneObjects`、`EnsureSceneObject` / `FindSceneObject` / `DestroySceneObject`、
  parse 时 `FillSceneObjectIdentity`

`14ef8c1` 线的字段（basis、`parallax_depth`、`FetchDest`、`ApplyAttachZeroOrigin`、`dest_size`、
`Flag304Bit4`、leftover / image_490 / lastpass / postfx mesh、`objectList`）在本线**不存在**。
是否引入要随对应机制（dest-STACK / `DestDrawPhase`）一起决定，不要提前加空字段。

收拢进度（不要再在 `Scene` 上平行一份）：

| 散落身份 | 迁到 | 状态 |
|---|---|---|
| `layerLocalVisibility` | `SceneObject::LocalVisible` | 完成（`f58a863`） |
| `layerParentBindings` | `SceneObject::SetParentBinding`（入口仍是 `Scene::SetLayerParentBinding`） | 完成（`f58a863`） |
| `imageLayers` 的 size/alignment | `SceneObject::ImageRuntimeState` | 完成（`f58a863`） |
| image passthrough | `SceneObject::Passthrough` | 完成（`f58a863`） |
| `nodeOwners[FinalNode] = layer_id` | 摘除，`ID()` 回指经 `NodeLayerId` 解析 | 完成（`457cc96`） |
| `nodeOwners[source / effect pass] = layer_id` | 摘除。effect pass node 设 `ID()` 回指并命名 `<name>::__hanabi_effect_pass_<eff>_<mat>`；parallax 回退经 `FindImageEffectLayer` 补到 pass node；`FindOwningLayerId` 加 `ID()` 回退 | 完成（本增量） |

`SceneNode::{m_localVisible,m_layerVisible}` 仍在 node 上：那是可见性解析写进 draw handle 的
结果状态，不是第二份 authored 可见性，保留。

层解析 helper 是 `Scene::LayerIdForNode`：`8e4c6cc` 起直接读 `SceneNode::ID()`（注册表已退役），
否则读 `SceneNode::ID()`；拿到 id 后用 `Scene::FindSceneObject(id)` 取对象。
图、parser、脚本宿主的三个旧入口（`NodeLayerId` / `ResolveLayerIdForRuntimeNode` /
`FindOwningLayerId`）全部委托给它。不要把 phase node 再登记成第二套身份。

### 2.2 拆掉多余的 node **种类**（不是删掉所有 node）

现在用它们表示「同一层的另一种画法」。
迁完之后它们应变成 **phase / pass 选项**，不再是场景图里的第二种 object。

| 多余身份 | 现在 | 迁完 |
|---|---|---|
| WorldNode | parser 的 `spWorldNode`（本线没有 `LayerNode()` 改名） | 就是这层的主 `SceneNode` handle。不要「世界一份、source 一份」 |
| source node + 每层私有相机 | `spImgNode` 已改名 `<name>::__hanabi_effect_source`（`091a6e1`）、相机已按层命名 `__hanabi_effect_camera_<id>`（`9899c75`）、已不进 `nodeOwners`、相机身上的 `m_imgEffect` 反向引用已删（`45104ad`）、source 已由 bridge 持有且**不再进 `sceneGraph`**（`f67b987`：world node 访问时经 bridge 路由发射） | 相机本体**留在** `Scene::cameras` 当纯投影资源（与 RT 同裁决，见 §2.5 修订）。source 已是 bridge 拥有的 draw handle（与 FinalNode / effect pass node 同款契约），场景图身份已消 |
| `FinalNode` | `m_final_node`，`CopyTrans`，`ID() = layer_id` 回指；不进 `nodeOwners` / `sceneGraph`，bridge 拥有 | **已收口**（§5 第 3 条修订）：与 effect pass node 同款受祝福 draw handle——发布 pass 的变换槽。真删依赖 pass 级变换覆盖，记中期项 |
| `leftover_mvp_node` | **本线不存在**（`EnsureLeftoverMvpNode` 是 `14ef8c1` 线的做法） | 本线无动作；若将来引入 `DestDrawPhase`，标在已有 handle 上，不 new node |
| `detachedEffectSourceNodes*` / `renderOrderProxy*` | **两组表全部已删**。detached：`f67b987`（source 归 bridge，不进树）；proxy：`7c72b34`（路由改为查询时从 authored 绑定推导——根挂 + `parent_id != 0` + attachment 空，register/restore/scrub 三个维护函数一并消失） | 已达成：顺序只看 `Scene::layerOrder` / 作者绑定。两个过期表角落顺带修正（脚本 reparent 跟随新父、前向引用父层正常路由） |
| `compose_source_camera` 当层身份 | `SceneToRenderGraph` 一路往下传 | 留成 route 上的 **RT / 相机选项**，不要变成第四种 node |

`SceneImageEffectNode::sceneNode` 可以留：那是 effect pass 的 draw handle，
**不是** authored object。规则（本增量已实现）：永不写入 `nodeOwners`；`ID()` 只作回指
（和 `FinalNode` 一样），名字用 `<name>::__hanabi_effect_pass_<eff>_<mat>`。

### 2.3 dest-draw 用 phase，不用兄弟姐妹（`14ef8c1` 线的机制，本线尚未引入）

`DestDrawPhase` / `ComposeDrawWalker` / dest-STACK（`DestStackPushCopy` /
`DestStackApplyPathB` / `DestStackPop`）/ 卡槽（leftover `0..AABB`、`image_490_mesh`、
last-pass ±half dest）在本线**都不存在**。若引入，按下述规则：

Walker 只 **写状态**（活 mesh、named-RT 尺寸、要上传的矩阵）。
GPU 仍在编译图循环里 execute——见 §3.1。

不要做：为每个 phase new 一枚 `SceneNode` 再 `AppendChild` 进 `sceneGraph`。

### 2.4 变换所有权：2D dest-draw 以 object 为准（依赖 §2.3，本线尚未引入）

`FetchDest` / dest-STACK 在本线不存在；目前 2D 放置仍是 `SceneNode` 变换。
若引入，迁移方向：

- image / text 的 origin、parent、attach、parallax 只写 `SceneObject`
- `WPNodeTransformResolver` 对这类 object 读 `FetchDest` / dest-STACK，不再把 `ModelTrans` 当 dest
- 层 handle 的 `Translate/Rotation/Scale`：要么单向从 object 同步，要么不再当放置源
- `AlignmentOffset` 留在 mesh / 卡上，不要再复制一份到 `FinalNode`

3D model / particle / light 可以继续用 `SceneNode::ModelTrans`。
不要强行把它们塞进 dest-STACK。

### 2.5 相机：三种场景相机 + 私有 pass 选项

结构上只承认：

1. 场景 / 窗口相机（fit-ortho、`global` / perspective）
2. leftover dest-ortho（named-RT，`0..W, 0..H`）
3. dest-STACK 上的当前 dest

**修订（`45104ad`）**：原计划「相机本体搬出 `Scene::cameras`、降级为 pass 描述」不再执行。
理由：相机与 render target 同构（按名字解析的投影/像素资源），而 RT 的裁决（`2e91ec4`）
是本体留全局池、bridge 只记名字；把相机本体搬走会逼 `WPShaderValueUpdater` /
`SceneToRenderGraph` / `VulkanRender` / `ParticleSystem` 长出双路查找，复杂度不降反升。

「相机当第二套身份」的真正病灶已在 `45104ad` 剥除：

- `SceneCamera::m_imgEffect`（相机持有 effect bridge 的 shared_ptr）已删；
  bridge 改由 `SceneObject` 直接持有（sound handle 同款所有权）
- 「node 的相机名 → 查相机 → 有没有挂 effect」这条把相机名当身份键的解析路径已改为
  「按层解析 bridge + 比对 `BridgeCameraName()`」（`ToGraphPass`、`WPShaderValueUpdater`
  两处、resize / 文本几何同步）
- 相机命名已按层收敛（`9899c75`：`__hanabi_effect_camera_<id>` 等）

现状：`Scene::cameras` 里的每层私有相机只是命名投影资源，无身份、无反向引用，
生命周期由 destroy 路径按 bridge 的 `RuntimeCameraNames()` 清理。

`__hanabi_model_perspective`、bloom/HDR 相机同样保留：它们不是「一层图的分身」。

### 2.6 解析合同（小、已经在做、继续）

这些只整理数据，不改 dest 代数：

- `FillSceneObjectIdentity` + `BindSceneObjectParent`
- `ApplyParse304Bit4` / `ApplyPuppet304Bit4`
- effect `visible` 的 `{script, value}`
- FIF mip 垫到 header 里的 physical 尺寸（content/physical UV）
- 卡选择：无 effect 用 `image_490_mesh`，有 effect 的 leftover 用 `0..AABB`

---

## 3. 保持 Vivid 原状

这些是 `bd0f775` 能画完 workshop 的原因，当 Vulkan / TREE 合同留下。

### 3.1 编译 render-graph，而不是 walker 里 Draw

现状：`compileRenderGraph` → `m_passes`（PrePass 先、FinPass 后）→ staging / TextureCache 再 execute。
已经改回「phase 只打标签、execute 一次、和 particle/shape/model 交错」。
**不要**再把 DestDraw 拉回 walker 里抢在 upload 前画。

`Record()` 在 dest-STACK live 时把矩阵/卡写进 pass，然后图循环去 Draw——这就够。

### 3.2 `FinalComposite` + `BLENDMODE` 挂在 composite material 上

owner material 保持中性，方程在 `ConfigureEffectFinalComposite`。
bridge-only / leftover-MVP 也走这条路。

**留这条代数。** 要迁的是结构：不要 `FinalNode` 这枚同 id `SceneNode`。
composite 可以是 `SceneImageEffectLayer` 上的 material + 一次 `AddNodePass(层 handle, phase=LeftoverMvp 或 Publish)`。

`SourcePolicy` / `HiddenFinalCompositePolicy` / hidden-effect bypass copy /
`runtime_visibility_contract` 都留——图必须显式旁路，否则会采到上一帧。

### 3.3 dest 进 `g_MVP`（`LastPassDrawMvp = LastPassMvp * FetchDest`）

字形 / 卡是 object-local ±half。FetchDest 乘进上传矩阵，Clock 和 Date 才能待在作者 dest 上。

**留。** 包括：

- Clock leftover FullFB 用 compose ±half 页 + `LastPassDrawMvp`
- Date/Day VERTICAL last-pass 同样
- 无 effect 的 image leftover / image last-pass 也走 `LastPassDrawMvp`

named-RT leftover 继续 dest-ortho × `0..AABB` 字形。
那是离屏 RT 的局部坐标，不是又一次「把 dest 塞进 MVP」。

在字形仍是 object-local 之前，不要把 Clock 的 upload 改回「只有相机、没有 FetchDest」。

### 3.4 私有 ping-pong、effect 相机、puppet surface

离屏链靠：

- `_rt_imageLayerComposite_*` / `ppong_a` / `ppong_b`
- effect 相机（层尺寸或 compose source 尺寸）
- `PuppetSurfaceProjection` + puppet surface 相机 / RT
- `Query` / `MarkShareReady`，以及 leftover 重绑后让 POSTFX / leftover-MVP / last-pass 一起 re-Query

**整套留。** 只把「相机/RT 名」从「假 SceneNode 身份」降成 layer 上的资源描述。
不要改成「全程画 Default、没有私有链」——puppet / blur / compose 都靠这条链。

### 3.5 文本：Pango + 两套 glyph page

Pango、atlas、`leftover_glyph_pages` vs compose `glyph_pages`。
`TextPass`、crop/alignment、`ApplyTextDestSize` 保持现状。
不要为了换一套顶点布局去重写排版。

### 3.6 alignment、compose 子层、passthrough

`SceneNode::AlignmentOffset`、`RefreshAlignedLayerPivot`、compose `SourcePolicy`
（Owner / Owner+proxy children / children only）是 `bd0f775` 的放置合同。

**行为留现状。** 结构上可以把 offset 收到 object 或 mesh，但不要改公式。
`passthroughLayerIds` 可收成 object flag，子层画进父 compose RT 的图路由留着。

### 3.7 非 dest-draw 的一切

particle、shape、model chunk、shadow、volumetrics、bloom/HDR（`__hanabi_scene_*`）、
`ShaderDrawCore` / `WPShaderValueUpdater::UpdateUniforms`、
`renderOrderProxy` 里真正的 3D/粒子附加 node——保持现状。

图顺序已经和 dest-draw 交错。不要用 dest-STACK 去「统一」模型矩阵。

### 3.8 Vulkan / 设备管道

没有 external-semaphore 也能 offscreen、TextureCache、FIF 上传、
staging 在 execute 之前——与对象结构无关，保持现状。

---

## 4. 明确不要改的（会再次把壁纸画坏）

上一轮已经验证过：改这些会回到 `14ef8c1` 刚落地时的损坏。

| 不要做 | 原因 |
|---|---|
| Clock/Date `g_MVP` 只传 `LastPassMvp`（没有 FetchDest） | 字形仍是 object-local，会掉到 fit-ortho 原点角 |
| 删 `FinalComposite`，在 owner 上直接编 `BLENDMODE` | parser 把 owner 留成中性；3219908811 层 600 会 ONE/ZERO 盖灰 |
| leftover / last-pass 再拆成 WorldNode 与 FinalNode 两套相机 | 这正是要删的种类；代数可以仍走 composite，身份不要拆 |
| DestDraw 在 walker 里、upload 之前 execute | z-order 和脏 mesh/贴图 |
| 用 `leftover_suppress` / inherit-heal / source-camera subtract 当放置 | 旧放置代数，且和现在 dest-STACK 叠床架屋 |
| 为了换顶点布局改 Pango | 排版合同是 Vivid 的 |
| 删 ping-pong / effect 相机，改全程 Default | compose、blur、puppet surface 会断 |

结构可以叫 leftover / last-pass；
`LastPassDrawMvp`、`FinalComposite` 标明是 **Vivid 合同**。

---

## 5. 建议顺序（每次只收一种身份）

不要开一个「删光 node」的大 PR。每步都用 `bd0f775` 的 workshop 观感验收
（至少 3219908811、3363252053）。

1. **冻结代数**——本线的基线就是 `bd0f775` 的绘制代数，只动所有权。（生效中）

2. **`SceneObject` 成为可见/parent/图层状态的唯一源**——`layerLocalVisibility` /
   `layerParentBindings` / `imageLayers` 已收进 object（`f58a863`）。
   **余项**：变换。object 上的 origin/scale/angles 还只是 parse 快照，
   脚本和运行时放置仍写 `SceneNode`。「脚本和 resolver 写 object」是
   dest-STACK 线（§2.3/§2.4，`14ef8c1`）的配套改动，本线不单独执行——
   在 node 变换仍是放置源的前提下单方向切换写入端只会制造双源。

3. **phase node 退出 `nodeOwners`**——FinalNode（`457cc96`）、detached source 与
   effect pass node（本增量）都已摘除，`ID()` 回指 + `NodeLayerId` 类解析
   （`8e4c6cc` 后注册表整个退役，回指只剩 `ID()` 一条路）。

   **修订（FinalNode 收口）**：原目标「删 FinalNode 这枚 node、改成
   `AddNodePass(层 handle)`」不再执行。理由与 §2.5 相机修订同构：身份危害
   （进 `nodeOwners`、进 `sceneGraph`、被当第二套 authored id 解析）已全部清零，
   FinalNode 现在与 effect pass node 是同一种受祝福形态——bridge 拥有的
   **pass 变换槽**（发布 pass 需要一个可能异于层 handle 的矩阵：
   `SyncResolvedNodeToMatrix` 写 route 解析结果，不能污染层 handle 的 authored 变换）。
   真删需要给 pass 层加变换覆盖机制（`NodePassOptions` 有 camera override 先例，
   技术上可行），动的是发布代数心脏，结构收益趋零——记为「有先例可循的中期项」，
   仅当 pass 级变换覆盖因其他需求落地时顺带做。

4. ~~删 `leftover_mvp_node`~~ **不适用本线**：这枚 node 和 `EnsureLeftoverMvpNode`
   只存在于 `14ef8c1` 线。

5. **相机剥离身份职责（`45104ad` 完成，方案见 §2.5 修订）**  
   `SceneCamera::m_imgEffect` 已删，bridge 归 `SceneObject` 持有，
   四处「相机名当身份键」的消费点改为按层解析 + `BridgeCameraName()` 比对。
   相机本体按 RT 同款裁决留在 `Scene::cameras` 当纯资源，不再降级搬家。
   **完成**：source node 已由 bridge 持有、不进 `sceneGraph`，
   `detachedEffectSourceNodes*` 两张表已删（`f67b987`）；`renderOrderProxy*`
   两张表也已删，路由改为查询时从 authored 绑定推导（`7c72b34`）。

6. **清命名**  
   `SyncResolvedNodeToWorld`、注释里的 WorldNode。`spWorldNode` 就是层 handle。

7. **可选：effect 内部 `sceneNode` 不进 `sceneGraph`**——本线已然如此：
   effect pass node 从不 `AppendChild` 进 `sceneGraph`，只经 `imgEffect->nodes`
   进 compile 列表。无需动作。

每步的检查：`sceneObjects` 一层一枚；`NodeLayerId` / `ResolveLayerIdForRuntimeNode` /
`FindOwningLayerId` 对任意 draw handle 解析稳定；
层解析只读 `SceneNode::ID()`（`nodeOwners` 已退役，`8e4c6cc`）；
phase node（FinalNode / source / effect pass）与层 handle 一律 `ID()` 回指。

---

## 6. 文件级对照（动手时从这里拆）

| 方向 | 文件 | 状态 |
|---|---|---|
| 加深 object | `SceneObject.h`、`Scene.h/.cpp`（`sceneObjects`、accessor） | `f58a863` 完成本层 |
| 收 parent / visible / 图层状态 | `WPSceneParser.cpp` `FillSceneObjectIdentity`、`WPSceneScriptHost.cpp` | `f58a863` 完成 |
| phase node 退出 `nodeOwners` | `WPSceneParser.cpp`（source / effect pass / FinalNode 登记点）、`SceneToRenderGraph.cpp` `NodeLayerId`、`WPSceneScriptHost.cpp` `FindOwningLayerId` | `457cc96` + 本增量完成 |
| 变换只读 object | `WPNodeTransformResolver.cpp`、`WPShaderValueUpdater.cpp`（dest-draw 层） | 未做（§5.2 余项） |
| 删 FinalNode 这枚 node | `SceneImageEffectLayer.*`、`ConfigureEffectFinalComposite`、`SceneToRenderGraph.cpp` `ToGraphPass(..., FinalNode)` | **收口不执行**（见 §5 第 3 条修订：身份危害已清零，node 是 bridge 内部 pass 变换槽；真删依赖 pass 级变换覆盖机制，记中期项） |
| 相机剥离身份职责 | `SceneCamera.h`（`m_imgEffect` 已删）、`SceneObject.h`（bridge 所有权）、`SceneImageEffectLayer.h` `BridgeCameraName`、`ToGraphPass` / `WPShaderValueUpdater` / resize / 文本几何四处消费点 | `45104ad` 完成（本体留 `Scene::cameras`，见 §2.5 修订） |
| 删 `detachedEffectSourceNodes*`（source 归 bridge） | `SceneImageEffectLayer.h` `DetachedSourceNodes`、`WPSceneParser.cpp` 注册点、`SceneToRenderGraph.cpp` 路由、`UpdateSceneLightingUniforms` | `f67b987` 完成 |
| 删 `renderOrderProxy*`（改派生查询） | `Scene.h/.cpp` `IsRenderOrderProxyNode` / `RenderOrderProxyChildrenOf`、`WPSceneParser.cpp`（register/restore/scrub 删除）、`SceneToRenderGraph.cpp` | `7c72b34` 完成 |
| 保持不动 | `TextPass.cpp` 排版、`WPTextLayer.cpp`、`WPTexImageParser.cpp` pad、`ShaderDrawCore.cpp` 非 dest-draw、bloom/HDR 合成 node | — |

---

## 7. 验收（结构）

迁对了：

- `scene.json` 每个 object ↔ 恰好一个 `SceneObject`，且在 `Scene::sceneObjects` 里
- 层回指只有一条路：`SceneNode::ID()`（`nodeOwners` 已退役）；phase node（source / effect pass / FinalNode）
  只靠 `ID()` 回指
- 绘制阶段最终是 phase，不是 `sceneGraph` 里的兄弟姐妹（source node 已出树归 bridge，
  `f67b987`；proxy 路由已改为绑定派生查询，`7c72b34`——node-keyed 顺序表全部消失）
- 每层私有相机不再当第二套 object（`45104ad`：无身份、无反向引用，本体按 RT 同款裁决
  留在 `Scene::cameras` 当纯投影资源）
- 3219908811：灯笼、Clock/Date 位置、层 600 subtract、vinyl/Frequency、前花 UV 与 `bd0f775` 同级（允许小误差，不允许掉角、灰块盖画、盘爆成白方）

迁错了（立刻停）：

- 又引入 WorldNode/source/Final 三套带同一 id 的 node
- 改 `LastPassDrawMvp` / 去掉 FinalComposite 方程
- DestDraw 重新在 walker 里 execute
