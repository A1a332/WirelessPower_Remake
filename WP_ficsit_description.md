# 标题（Name，按需调整）

Wireless Power — Reborn

> 说明：若 Feyko 已同意你接手原页，标题保持 **Wireless Power** 即可；若走新页，建议加 "Reborn" / "Redux" 之类后缀，避免和原页混淆。

---

# 短描述（Short Description，一句话）

Wireless power transfer for Satisfactory — peer nodes share one power circuit across distance, no cables. A modern reimplementation of the classic Wireless Power concept.

---

# 长描述（Long Description，支持 Markdown，点 "View" 时显示）

## What it does

**Wireless Power** lets you move electricity without running cables. Place peer nodes that join one shared power circuit automatically — machines in range draw power as if they were wired, even across a canyon or a long base.

This is a from-scratch reimplementation of the wireless-power idea for **modern Satisfactory** (SML / Satisfactory 1.x). The original *Wireless Power* by Feyko (ficsit.app) has been unmaintained since v0.0.7alpha (Feb 2022) and does not run on current versions; this mod revives the concept with a cleaner, fully C++ implementation.

## How it works

- **No IDs required to get started.** A newly placed node automatically joins the in-range grid and inherits the nearest node's channel. Nodes sharing a channel and standing within range link up by themselves.
- **One circuit, no cables.** Same-channel nodes within range share power — the "wire" is the air between them.
- **Language follows the game.** Chinese players see Chinese; everyone else sees English.

## Buildings

| Building                         | Range / Note                                                                                                                                                                     |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Power Relay Mk1 / Mk2 / Mk3**  | Access point for the wireless grid. Wire it into a powered grid; it shares power with same-channel nodes within **60 m / 120 m / 160 m**.                                        |
| **Power Converter**              | Bridges a *wired* grid and the *wireless* grid. Once wired into a powered grid, it supplies same-channel nodes within **48 m**. (Mk2 / Mk3 are reserved / disabled for now.)     |
| **Power Feeder Mk1 / Mk2 / Mk3** | Connect your machines here; it draws power from same-channel nodes within **12 m / 24 m / 48 m**.                                                                                |
| **Power Transmission Tower**     | Long-range link up to **4800 m**; towers need line of sight to each other. Carries power across canyons and long distances. Can also be paired manually by ID via "Toggle link". |

## Console (Wireless Power Console)

Open it on any node to:

- Set / view the node **ID (channel)** and link it to nearby peers.
- Adjust **coverage radius** (exact value, in metres).
- **Power on / off** the node.
- Manage **tower linkable IDs**, toggle **beam visibility** and **range display**.
- Read a live **status** line: powered / no-power, nodes linked, machines powered.

## Milestones

- **Wireless Power** — unlocks the Relay Mk1 (60 m), Power Converter and Power Feeder Mk1.
- **Wireless Power Mk2** — unlocks the Relay Mk2 (120 m) and Feeder Mk2 (24 m).
- **Wireless Power tier 3** — unlocks the **Power Transmission Tower** (4800 m line-of-sight link) and the Relay Mk3 (160 m) / Feeder Mk3 (48 m).

## Compatibility

- Requires **SML ^3.12** (Satisfactory Mod Loader).
- Built and tested on **[Satisfactory 1.1 / 1.2 — confirm exact version before publishing]**.
- Multiplayer-friendly (shared channel = shared protocol version).

## Credits

- Original concept & inspiration: **Feyko** — *Wireless Power* (ficsit.app, last updated Feb 2022, since unmaintained). This mod is an independent reimplementation and is not affiliated with or endorsed by the original author.
- Reimplementation & maintenance: **[YourName]** ([YourDiscordTag]).

## Get support / report bugs

- Discord: **[YourDiscordTag]**
- Issues: **[GitHub issues link or alternative]**
- Source code: **[GitHub repository link]**

---

# 备注（不发布，仅给你自己看）

- `mod_reference`：接手原页 → 必须和 Feyko 原页完全一致（去他 ficsit 页查）；新页 → 建议换不撞车的名字（如 `WirelessPowerReborn`）。
- 上架前我帮你把 `mod.json` / `.uplugin` 的 version、SML 依赖、gameVersion、开源链接、作者字段一次性理干净。
- 上面 `[Satisfactory 1.1 / 1.2 ...]` 占位符：等我们核完 1.2 兼容再填确切值。
