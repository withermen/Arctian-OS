#ifndef ARCTIAN_OOM_UI_H
#define ARCTIAN_OOM_UI_H

void oom_ui_init(void);
void oom_ui_tick(void);
void oom_ui_enter(int manual);
void oom_ui_exit(void);
int  oom_ui_is_open(void);
void oom_ui_reset_after_rebuild(void);

#endif /* ARCTIAN_OOM_UI_H */
