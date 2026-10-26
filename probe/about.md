# gddiff Window Probe

Measures how wide the timing window of every click in a macro is.

For each click the mod shifts it earlier and later, tick by tick (with a fast
search), keeping all other clicks as in the macro, and checks whether the
player survives. The furthest shifts that still survive are the window edges.

1. Convert a GDR JSON macro with `gddiff probe-prepare` and put the resulting
   `probe_input.txt` into this mod's save folder.
2. Turn on **Probe on level start** and open the level. The mod switches to
   practice mode and measures. Your own clicks are ignored while it runs.
3. Results are written to `probe_<levelID>.csv` in the same folder. Feed them
   to `gddiff windows`.

The mod never completes a level in normal mode and never saves progress.
