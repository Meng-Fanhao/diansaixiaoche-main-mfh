/**
 * @file    common.h
 * @brief   通用工具宏（全工程共享）
 */

#ifndef COMMON_H
#define COMMON_H

#define CLAMP(val, lo, hi)  (((val) < (lo)) ? (lo) : (((val) > (hi)) ? (hi) : (val)))

#endif /* COMMON_H */
