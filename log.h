// Copyright(c) 2020 Intel Corporation. All Rights Reserved.
#pragma once

#define LOGI(...) do { fprintf(stdout, "INF: "); fprintf(stdout,__VA_ARGS__); fprintf(stdout,"\n"); } while (0)
#define LOGE(...) do { fprintf(stdout, "ERR: "); fprintf(stdout,__VA_ARGS__); fprintf(stdout,"\n"); } while (0)
#define LOGD(...) do { fprintf(stdout, "DBG: "); fprintf(stdout,__VA_ARGS__); fprintf(stdout,"\n"); } while (0)