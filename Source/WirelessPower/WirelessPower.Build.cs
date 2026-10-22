using UnrealBuildTool;

public class WirelessPower : ModuleRules {
    public WirelessPower(ReadOnlyTargetRules Target) : base(Target) {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        // v1.0.52: 编号改为「中文汉字」。本项目源码以 UTF-8 保存，且源码里直接出现中文字面量
        // （如 TEXT("一号")、面板标签）。MSVC 默认按**本地代码页**(简体中文 Windows = GBK/936)
        // 解释窄字符源文件，中文会被误解成乱码，且这种错误只在**运行时**才暴露出来（编译不报错）。
        // /utf-8 同时设置「源字符集」与「执行字符集」为 UTF-8，是 MSVC 上唯一可靠的做法。
        // 这里**不需要**任何额外设置：UBT 已经无条件加 /utf-8（见文件末尾说明），
        // 而 bEnableExceptions / UNICODE 宏跟源文件编码毫无关系。
        PrivateDefinitions.Add("_CRT_SECURE_NO_WARNINGS");

        PublicIncludePaths.AddRange(new string[] {
            // SML 头文件路径（如与默认布局不同，请按你的 SML 安装位置调整）
            // ModuleDirectory + "/../../Source/SML/Include"
        });

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "FactoryGame",   // Satisfactory 游戏模块：AFGBuildable / UFGPowerConnectionComponent / AFGBuildableSubsystem
            "SML",           // Satisfactory Mod Loader
        });

        PrivateDependencyModuleNames.AddRange(new string[] {
            "Projects",
            "UMG",          // C++-built control panel (UFGInteractWidget subclass)
            "Slate",
            "SlateCore"
        });

        // —— 编辑器专属依赖（仅 FactoryEditor 目标，严禁进入 Shipping）——
        // WirelessPowerRenameContentCommandlet 用 IAssetTools::RenameAssets 重写内容包路径；
        // AssetTools 是 Developer 编辑器模块，Shipping 构建链中没有它 → 必须 bBuildEditor 守卫，
        // 否则 PackagePlugin(Shipping) 链接期报 LNK1120。命令列跑完后会 .bak 移出构建（双保险）。
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] {
                "AssetTools",
                "AssetRegistry"
            });
        }

        // 中文源码字面量：需要 MSVC 的 /utf-8（源 + 执行字符集都是 UTF-8），但**不需要**在这里手动加。
        // UE5 的 VCToolChain.cs 对 MSVC 无条件加 /utf-8（Engine/Source/Programs/UnrealBuildTool/
        // Platform/Windows/VCToolChain.cs:650 Arguments.Add("/utf-8")）。
        // 曾经写过的 AddCompilerFlagIfSupported 在这个 UBT 版本里并不存在（CS0103），已移除。
    }
}
