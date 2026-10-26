import numpy as np
import pandas as pd
import pytest
from scipy.optimize import check_grad

from gddiff.eval.cv import cross_validate
from gddiff.eval.residuals import section_outcomes, visibility_check
from gddiff.io.dataset import load_dataset
from gddiff.models.baseline import ModelConfig, TimingModel, make_pairs, pack
from gddiff.pipeline import build_tables
from gddiff.synth import generate


def _random_tables(rng, n_levels=8):
    tables = {}
    modes = ["cube", "ship", "wave", "unknown"]
    for lid in range(n_levels):
        n = int(rng.integers(20, 60))
        tables[lid] = pd.DataFrame(
            {
                "t_ms": np.sort(rng.uniform(0, 20000, n)),
                "section": np.sort(rng.integers(0, 4, n)),
                "window_ms": rng.uniform(5, 300, n),
                "mode": rng.choice(modes, n),
                "speed": rng.choice([0.5, 1.0, 2.0], n),
                "density": rng.uniform(0, 10, n),
                "position": rng.uniform(0, 1, n),
                "down": rng.random(n) < 0.5,
            }
        )
    return tables


@pytest.mark.parametrize("form", ["power", "exp"])
def test_gradient_matches_finite_differences(form):
    rng = np.random.default_rng(1)
    tables = _random_tables(rng)
    model = TimingModel(ModelConfig(form=form))
    P = pack(tables, model.config.w_min_ms)
    a, b = make_pairs(rng.permutation(len(tables)) + 1.0, 10_000)
    v = model._init() + rng.normal(0, 0.3, model._init().shape)
    f = lambda x: model._loss_grad(x, P, a, b)[0]
    g = lambda x: model._loss_grad(x, P, a, b)[1]
    err = check_grad(f, g, v, epsilon=1e-6)
    assert err < 1e-4 * (1 + np.linalg.norm(g(v)))


def test_pairs_put_harder_level_first():
    a, b = make_pairs(np.array([3.0, 1.0, 2.0]), 100)
    pos = np.array([3.0, 1.0, 2.0])
    assert (pos[a] < pos[b]).all() and len(a) == 3


@pytest.fixture(scope="module")
def synth_data(tmp_path_factory):
    root = tmp_path_factory.mktemp("synth")
    generate(root, n_levels=60, n_macros=3, seed=3)
    return root, load_dataset(root)


def test_model_learns_ranking_on_synthetic(synth_data):
    _, records = synth_data
    tables, positions, skipped = build_tables(records, "spread")
    assert not skipped
    model = TimingModel(ModelConfig()).fit(tables, positions)
    assert model.fit_info["pair_accuracy"] > 0.8
    params = model.params()
    assert params["p"] >= 1 and params["q"] >= 1 and params["beta"] > 0
    sections = model.explain(next(iter(tables.values())))
    assert np.isclose(sections["share"].sum(), 1.0)


def test_cross_validation_runs(synth_data):
    _, records = synth_data
    tables, positions, _ = build_tables(records, "interval")
    cv = cross_validate(tables, positions, ModelConfig(maxiter=100), folds=3)
    assert set(cv["method"]) >= {"model", "clicks_per_second"}


def test_save_and_load(tmp_path, synth_data):
    _, records = synth_data
    tables, positions, _ = build_tables(records[:20], "interval")
    model = TimingModel(ModelConfig(maxiter=50)).fit(tables, positions)
    path = tmp_path / "model.json"
    model.save(path)
    again = TimingModel.load(path)
    s1, s2 = model.score(tables), again.score(tables)
    assert np.allclose(s1, s2)


def test_section_outcomes_counts_reach():
    df = pd.DataFrame({"t_ms": [0, 100, 1000, 1100], "x": np.nan, "section": [0, 0, 1, 1]})
    deaths = pd.DataFrame({"percent": [5.0, 95.0, 100.0], "deaths": [10, 5, 85]})
    out = section_outcomes(df, deaths)
    assert out["deaths"].tolist() == [10, 5]
    assert out["reached"].tolist() == [100, 90]


def test_visibility_check_finds_hidden_sections(synth_data):
    _, records = synth_data
    tables, positions, _ = build_tables(records, "spread", with_visibility=True)
    model = TimingModel(ModelConfig()).fit(tables, positions)
    res = visibility_check(model, tables, records, n_perm=100)
    assert res["p_value"] < 0.05
    assert res["coefficients"]["alpha_triggers"] > 0
