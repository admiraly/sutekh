#include <stdint.h>
#include <stdio.h>
#include "world/snapshot.h"
int main(void) { uint8_t byte=0; SuWorld *w=(SuWorld *)(uintptr_t)1; SuSnapshotData d; SuSnapshotError e; uint32_t s=su_snapshot_restore_world(&byte,(size_t)SU_SNAPSHOT_MAX_BYTES+1,NULL,1,&w,&d,&e); printf("status=%u expected=%u world_null=%d\n",s,SU_E_RESOURCE_LIMIT,w==NULL); return !(s==SU_E_RESOURCE_LIMIT && w==NULL); }
