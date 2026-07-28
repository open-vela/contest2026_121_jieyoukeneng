/****************************************************************************
 * 安聆 VelaGuard - 特征转储工具（主机侧）
 *
 * 用于验证端侧 C 实现（vg_feature.c）与训练脚本
 * （model/velaguard_features.py）提取的特征逐维一致。
 * 两者不一致会导致电脑端训练指标在板上不成立，因此这是必测项。
 *
 *   vg_featdump <file.wav>          输出每个 1 秒窗口的 40 维特征
 *   vg_featdump --classify <wav>    额外输出两个模型的分类结果
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "velaguard/vg_capture.h"
#include "velaguard/vg_classifier.h"
#include "velaguard/vg_feature.h"

int main(int argc, char *argv[])
{
  static int16_t pcm[VG_WINDOW_SAMPLES];
  const char *path;
  bool classify = false;
  int window = 0;

  if (argc < 2)
    {
      fprintf(stderr, "用法: vg_featdump [--classify] <file.wav>\n");
      return 1;
    }

  if (strcmp(argv[1], "--classify") == 0)
    {
      classify = true;
      if (argc < 3)
        {
          fprintf(stderr, "缺少 wav 路径\n");
          return 1;
        }

      path = argv[2];
    }
  else
    {
      path = argv[1];
    }

  if (vg_capture_open(VG_SRC_WAV, path) < 0)
    {
      return 1;
    }

  while (vg_capture_read(pcm, VG_WINDOW_SAMPLES) == VG_WINDOW_SAMPLES)
    {
      float feat[VG_FEATURE_DIM];
      int i;

      if (vg_feature_extract(pcm, VG_WINDOW_SAMPLES, feat) < 0)
        {
          break;
        }

      printf("window %d", window++);
      for (i = 0; i < VG_FEATURE_DIM; i++)
        {
          printf(" %.6f", (double)feat[i]);
        }

      if (classify)
        {
          float ep[VG_ENV_CLASSES];
          float vp[VG_VOICE_CLASSES];
          vg_sound_class_t cls = vg_classify_env(feat, ep);
          vg_voice_kind_t kind = vg_classify_voice(feat, vp);

          printf(" | env=%s(%.2f) voice=%s",
                 vg_sound_class_str(cls), (double)ep[cls],
                 vg_voice_kind_str(kind));
        }

      printf("\n");
    }

  vg_capture_close();
  return 0;
}
