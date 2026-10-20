优化前的原始文件备份（2026-10-15）
=================================

这 5 个文件是 square_track 工程在"效率优化"改动之前的原文，原样保存，方便逐行对比或回退。
加了 .txt 后缀是为了让 IDE / clangd 别去解析它们（不然 Problems 面板里会冒一堆
"找不到 ti_msp_dl_config.h"）。这个目录不在任何 CCS 工程里（不在 square_track/ 下），
CCS 不会编译它，对比完可以整个删掉。

  main.c.txt                    → square_track\main.c
  oled_hardware_i2c.c.txt       → square_track\Hardware\display\oled_hardware_i2c.c
  oled_hardware_i2c.h.txt       → square_track\Hardware\display\oled_hardware_i2c.h
  board.c.txt                   → square_track\Hardware\system\board.c
  board.h.txt                   → square_track\Hardware\system\board.h

对照（装了 git 的话最省事，在仓库根目录下执行）：

  git diff --no-index --stat _backup_before_optimize_2026-10-15\main.c.txt square_track\main.c

或者用 PowerShell 逐行对比：

  Compare-Object (Get-Content .\_backup_before_optimize_2026-10-15\main.c.txt) `
                 (Get-Content .\square_track\main.c)

回退单个文件（注意把 .txt 去掉）：

  Copy-Item .\_backup_before_optimize_2026-10-15\main.c.txt .\square_track\main.c -Force
