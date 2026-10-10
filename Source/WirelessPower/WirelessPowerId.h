#pragma once

#include "CoreMinimal.h"

/**
 * 编号（ID）的规范化与比较 —— v1.0.52（阶段 A）。
 *
 * Feyko 原版用【字符串】当编号（一手证据：Relay 蓝图里的节点 EqualEqual_StriStri
 * = String==String；供电器面板明文字面量 "Set ID here"）。我们此前自创了 int32 数字编号，
 * 还为了回避"转换器编号"问题搞出 4 位二进制显示，属于偏离原版。本版还原为字符串，
 * 于是"同编号"= 字符串相等，汉字天然合法。
 *
 * 规范化规则（唯一入口，比较必须先规范化）：
 *   1. 去掉**首尾**空白（含全角空格 U+3000）；
 *   2. 全角 ASCII（U+FF01..U+FF5E）折半角，全角空格折半角空格；
 *   3. ASCII 字母转大写（与 Feyko 的 Stri 比较口径一致）。
 * 注意：**内部空格保留**，因为 Feyko 是直接字符串比较，我们不做超出原版的加工。
 * 空串是合法编号（= 默认网络），但为了不出现"看不见的编号"，面板层禁止提交纯空白。
 */
namespace FWirelessId
{
    /** 规范化：Trim + 全角折半角 + ASCII 大写。比较/存储一律走这里。 */
    inline FString Normalize(const FString& In)
    {
        FString S = In;
        S.TrimStartAndEndInline();

        FString Out;
        Out.Reserve(S.Len());
        for (int32 i = 0; i < S.Len(); ++i)
        {
            TCHAR C = S[i];
            // 全角空格 -> 半角空格
            if (C == 0x3000)
            {
                C = TEXT(' ');
            }
            // 全角 ASCII（！U+FF01 .. ～U+FF5E）-> 半角
            else if (C >= 0xFF01 && C <= 0xFF5E)
            {
                C = static_cast<TCHAR>(C - 0xFEE0);
            }
            if (C >= TEXT('a') && C <= TEXT('z'))
            {
                C = static_cast<TCHAR>(C - 32);
            }
            Out.AppendChar(C);
        }
        Out.TrimStartAndEndInline();
        return Out;
    }

    /** 两个编号是否属于同一网络（唯一判据）。 */
    inline bool Equal(const FString& A, const FString& B)
    {
        return Normalize(A) == Normalize(B);
    }

    /** 面板提交前的轻校验：不能是纯空白，且长度有上限。Out 返回规范化结果。 */
    inline bool Validate(const FString& In, FString& OutNormalized, int32 MaxLen = 24)
    {
        OutNormalized = Normalize(In);
        if (OutNormalized.IsEmpty())
        {
            return false;
        }
        // 规范化后按"码点"计数（中文字符不应按字节截断）。
        if (OutNormalized.Len() > MaxLen)
        {
            OutNormalized = OutNormalized.Left(MaxLen);
        }
        return true;
    }

    /**
     * 老存档迁移：v1.0.51 及以前 Channel 是 int32 数字（面板上显示 1/2/3，转换器为 4 位二进制），
     * 现在要变成字符串。**只做一次**的迁移规则：
     *   - 数字 n  -> 十进制字符串（"1" / "12"），与老面板上玩家看到的十进制一致；
     *   - 0       -> 空串（老版 0 是"默认网络"的语义）。
     * 迁移是幂等的：迁移后再跑 Normalize 结果不变，所以重复执行无副作用。
     * 只在"读到的值看起来像纯数字"时才会被调用（见 AWirelessPowerBuildable::MigrateChannelIds）。
     */
    inline FString FromLegacyInt(int32 Legacy)
    {
        return (Legacy == 0) ? FString() : FString::FromInt(Legacy);
    }

    /** 规范化后的显示文本；空编号显示为"—"（面板用）。 */
    inline FString ToDisplay(const FString& In)
    {
        const FString N = Normalize(In);
        return N.IsEmpty() ? FString(TEXT("—")) : N;
    }
}
