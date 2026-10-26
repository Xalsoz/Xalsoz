"""Командная строка: python -m gddiff <команда> ..."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd

from .eval.cv import cross_validate, summarize
from .eval.residuals import visibility_check
from .features.timing import WINDOW_METHODS, timing_table
from .io.dataset import LevelRecord, load_dataset
from .io.level import load_layout
from .io.lists import fetch_aredl, fetch_pointercrate, list_agreement
from .io.macro import load_macro
from .io.probe import probe_input_text, read_probe
from .models.baseline import FORMS, ModelConfig, TimingModel
from .pipeline import build_tables

pd.set_option("display.width", 140)
pd.set_option("display.max_columns", 20)


def _add_model_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--data", required=True, help="папка набора (levels.csv, macros/, layouts/)")
    p.add_argument("--window", choices=WINDOW_METHODS, default="interval", help="как оценивать окно допуска")
    p.add_argument("--form", choices=FORMS, default="power", help="форма f(w)")
    p.add_argument("--gap-ms", type=float, default=1000.0, help="пауза, после которой начинается новая секция")
    p.add_argument(
        "--smooth", type=int, default=None, help="медиана окна по стольким соседним таймингам (по умолчанию 5, для measured 1)"
    )
    p.add_argument("--seed", type=int, default=0)


def _config(args) -> ModelConfig:
    return ModelConfig(
        form=args.form,
        window_method=args.window,
        gap_ms=args.gap_ms,
        smooth_events=args.smooth,
        seed=args.seed,
    )


def _tables(args, records, with_visibility: bool = False):
    return build_tables(records, args.window, args.gap_ms, with_visibility=with_visibility, smooth_events=args.smooth)


def cmd_synth(args) -> None:
    from .synth import generate

    info = generate(
        args.out,
        n_levels=args.levels,
        n_macros=args.macros,
        seed=args.seed,
        label_noise=args.label_noise,
        coupling=args.coupling,
    )
    print(json.dumps(info, ensure_ascii=False, indent=2))


def cmd_cv(args) -> None:
    records = load_dataset(args.data)
    tables, positions, _ = _tables(args, records)
    cv = cross_validate(tables, positions, _config(args), folds=args.folds, seed=args.seed)
    print(f"уровней: {len(tables)}, окно: {args.window}, форма: {args.form}, фолдов: {args.folds}")
    print(summarize(cv).round(3).to_string())


def cmd_train(args) -> None:
    records = load_dataset(args.data)
    tables, positions, _ = _tables(args, records)
    model = TimingModel(_config(args)).fit(tables, positions)
    model.save(args.out)
    print(json.dumps({"fit": model.fit_info, "params": model.params()}, ensure_ascii=False, indent=2))


def cmd_explain(args) -> None:
    model = TimingModel.load(args.model)
    if not args.macro and not args.probe:
        raise ValueError("нужен --macro или --probe")
    record = LevelRecord(
        level_id=0,
        name=Path((args.macro or [args.probe])[0]).stem,
        position=None,
        macros=[load_macro(p) for p in args.macro or []],
        layout=load_layout(args.layout) if args.layout else None,
        probe=read_probe(args.probe) if args.probe else None,
    )
    df = timing_table(
        record,
        window_method=model.config.window_method,
        gap_ms=model.config.gap_ms,
        smooth_events=model.config.smooth_events,
    )
    sections = model.explain(df)
    score = float(model.score({0: df}).iloc[0])
    print(f"сложность (log D): {score:.3f}")
    print(f"примерная позиция среди уровней обучения: {model.estimate_position([score])[0]:.0f}")
    print(sections.round(3).to_string(index=False))


def cmd_visibility(args) -> None:
    records = load_dataset(args.data)
    tables, positions, _ = _tables(args, records, with_visibility=True)
    model = TimingModel(_config(args)).fit(tables, positions)
    res = visibility_check(model, tables, records, n_perm=args.perm, seed=args.seed)
    coefs = res.pop("coefficients")
    print(json.dumps(res, ensure_ascii=False, indent=2))
    print(coefs.round(3).to_string())


def cmd_probe_prepare(args) -> None:
    text, warnings = probe_input_text(load_macro(args.macro))
    Path(args.out).write_text(text, encoding="utf-8")
    for w in warnings:
        print(f"внимание: {w}", file=sys.stderr)
    print(f"вводов: {text.count(chr(10)) - 2} -> {args.out}")


def _parse_hz(values) -> list[float]:
    return [float("inf") if str(v).lower() in ("inf", "cbf", "subframe") else float(v) for v in values]


def cmd_windows(args) -> None:
    from .features.refresh import hit_probability, level_summary
    from .preprocess import assign_sections

    probe = read_probe(args.probe)
    ok = probe[probe["status"] == "ok"]
    if ok.empty:
        raise ValueError("в файле нет измеренных кликов")
    hz = _parse_hz(args.hz)
    w = ok["window_ms"].to_numpy(float)
    print(f"файл: {args.probe}" + ("" if probe.attrs.get("complete", True) else "  (замер прерван, данные неполные)"))
    print(
        f"кликов: {len(probe)}, измерено: {len(ok)}, упёрлись в предел поиска: {int(ok['censored'].sum())} "
        f"(их окна не меньше указанного)"
    )
    print(f"окно, мс: медиана {np.median(w):.1f}, 10-й перцентиль {np.quantile(w, 0.1):.1f}, минимум {w.min():.1f}")
    print(f"\nсложность при разбросе нажатия sigma = {args.sigma:g} мс (игрок целится в окно с поправкой на задержку кадра):")
    summary = pd.DataFrame(level_summary(w, hz, args.sigma)).set_index("hz")
    print(summary.round(3).to_string())

    table = ok[["index", "frame", "t_ms", "down", "window_ms", "censored"]].copy()
    table["t_s"] = (table["t_ms"] / 1000.0).round(2)
    table["action"] = np.where(table["down"], "нажатие", "отпускание")
    for h in hz:
        table[f"p_{'inf' if not np.isfinite(h) else f'{h:g}'}"] = hit_probability(table["window_ms"], h, args.sigma)
    print(f"\n{args.top} самых узких кликов:")
    cols = ["index", "t_s", "action", "window_ms", "censored"] + [c for c in table.columns if c.startswith("p_")]
    print(table.nsmallest(args.top, "window_ms")[cols].round(3).to_string(index=False))

    sections = assign_sections(probe["t_ms"].to_numpy(float), None, args.gap_ms)
    ref_hz = hz[0]
    p = hit_probability(probe["window_ms"], ref_hz, args.sigma)
    sec = pd.DataFrame({"section": sections, "t_s": probe["t_ms"] / 1000.0, "nll": -np.log(np.clip(p, 1e-300, 1.0))})
    sec = sec[np.isfinite(sec["nll"])]
    by = sec.groupby("section").agg(t_start_s=("t_s", "min"), t_end_s=("t_s", "max"), clicks=("nll", "size"), nats=("nll", "sum"))
    by["share"] = by["nats"] / by["nats"].sum()
    label = "inf" if not np.isfinite(ref_hz) else f"{ref_hz:g}"
    print(f"\nвклад секций в сложность при {label} Гц (паузы > {args.gap_ms:g} мс делят секции):")
    print(by.round(3).to_string())


def cmd_fetch_list(args) -> None:
    df = fetch_pointercrate() if args.source == "pointercrate" else fetch_aredl(url=args.url) if args.url else fetch_aredl()
    df.to_csv(args.out, index=False)
    print(f"сохранено уровней: {len(df)} -> {args.out}")


def cmd_agreement(args) -> None:
    print(json.dumps(list_agreement(pd.read_csv(args.a), pd.read_csv(args.b)), indent=2))


def main(argv=None) -> None:
    ap = argparse.ArgumentParser(prog="gddiff", description="Оценка сложности демонов GD по таймингам")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("synth", help="синтетический набор для проверки кода")
    p.add_argument("--out", required=True)
    p.add_argument("--levels", type=int, default=200)
    p.add_argument("--macros", type=int, default=3)
    p.add_argument("--label-noise", type=float, default=0.15)
    p.add_argument("--coupling", type=float, default=0.3, help="связь ритма с окном: 1 = полная, 0 = нет")
    p.add_argument("--seed", type=int, default=0)
    p.set_defaults(func=cmd_synth)

    p = sub.add_parser("cv", help="кросс-валидация модели и наивных оценок")
    _add_model_args(p)
    p.add_argument("--folds", type=int, default=5)
    p.set_defaults(func=cmd_cv)

    p = sub.add_parser("train", help="обучить модель и сохранить в JSON")
    _add_model_args(p)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_train)

    p = sub.add_parser("explain", help="разбор уровня по секциям")
    p.add_argument("--model", required=True)
    p.add_argument("--macro", nargs="+", help="один или несколько макросов уровня")
    p.add_argument("--probe", help="окна, измеренные модом (для модели, обученной с --window measured)")
    p.add_argument("--layout", help="строка уровня (.txt) или .gmd")
    p.set_defaults(func=cmd_explain)

    p = sub.add_parser("check-visibility", help="объясняют ли признаки видимости остатки модели")
    _add_model_args(p)
    p.add_argument("--perm", type=int, default=500)
    p.set_defaults(func=cmd_visibility)

    p = sub.add_parser("probe-prepare", help="макрос GDR JSON -> probe_input.txt для мода")
    p.add_argument("--macro", required=True)
    p.add_argument("--out", default="probe_input.txt")
    p.set_defaults(func=cmd_probe_prepare)

    p = sub.add_parser("windows", help="разбор окон, измеренных модом, и сложность по частоте кадров")
    p.add_argument("--probe", required=True, help="probe_<levelID>.csv из папки мода")
    p.add_argument("--hz", nargs="+", default=["60", "144", "240", "inf"], help="частоты кадров; inf = ввод между кадрами")
    p.add_argument("--sigma", type=float, default=10.0, help="разброс нажатия игрока, мс (допущение)")
    p.add_argument("--top", type=int, default=10)
    p.add_argument("--gap-ms", type=float, default=1000.0)
    p.set_defaults(func=cmd_windows)

    p = sub.add_parser("fetch-list", help="скачать позиции из списка")
    p.add_argument("--source", choices=("pointercrate", "aredl"), required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--url", help="адрес API AREDL, если отличается от стандартного")
    p.set_defaults(func=cmd_fetch_list)

    p = sub.add_parser("agreement", help="согласие двух списков на общих уровнях")
    p.add_argument("--a", required=True)
    p.add_argument("--b", required=True)
    p.set_defaults(func=cmd_agreement)

    args = ap.parse_args(argv)
    try:
        args.func(args)
    except (ValueError, FileNotFoundError) as err:
        print(f"ошибка: {err}", file=sys.stderr)
        sys.exit(1)
