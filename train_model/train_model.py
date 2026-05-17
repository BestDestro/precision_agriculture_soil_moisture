#!/usr/bin/env python3
import json
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import tensorflow as tf


FREQUENCIES = [
    "128mhz", "64mhz", "32mhz", 
    "16mhz", "8mhz", "4mhz", 
    "2mhz", "1mhz", "500khz"
    ]
FEATURES = FREQUENCIES + [
    f"std_{freq}" for freq in FREQUENCIES
    ] + ["temp"]

EPOCHS = 500
BATCH_SIZE = 8
LEARNING_RATE = 2e-3
EARLY_STOPPING_PATIENCE = 50
LR_PATIENCE = 12
RANDOM_SEED = 10

JSON_PATH = Path("data.json")
EXCLUDED_EXECUTION_INDICES: List[int] = list(range(64, 94))
TEST_SIZE = 0.15
VAL_SIZE = 0.15


# Busca el fichero data.json en la carpeta actual.
def resolve_json_path() -> Path:
    configured = Path(JSON_PATH)
    if configured.exists():
        return configured
    raise FileNotFoundError(f"No se encontro el dataset en {configured}")


# Convierte un valor del JSON a float. None si el campo no existe.
def _optional_float(value) -> Optional[float]:
    if value is None:
        return None
    return float(value)


# Lee el porcentaje de perlita aplicado en una ejecucion.
def perlite_percent_from_execution(execution: Dict) -> float:
    return _optional_float(execution.get("perlita_porcentaje_sobre_tierra")) or 0.0


# Calcula gramos de fosfato y ml de vinagre aplicados en una ejecucion.
def chemical_amounts_from_execution(execution: Dict) -> Tuple[float, float]:
    volume_used_l = _optional_float(execution.get("volumen_mezcla_utilizado_l")) or 0.0
    phosphate_concentration_g_l = _optional_float(execution.get("concentracion_fosfato_g_l")) or 0.0
    acidity_ratio = _optional_float(execution.get("proporcion_vinagre_en_mezcla")) or 0.0
    phosphate_applied_g = max(0.0, volume_used_l * phosphate_concentration_g_l)
    acidity_applied_ml = max(0.0, volume_used_l * acidity_ratio * 1000.0)
    return float(phosphate_applied_g), float(acidity_applied_ml)


# Carga los barridos validos y devuelve entradas, humedad, variables de split y grupo de ejecucion.
def load_samples_from_json(
    json_path: Path,
    features: Sequence[str],
    excluded_execution_indices: Optional[Iterable[int]] = None,
) -> Tuple[
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
    np.ndarray,
]:
    excluded = set(excluded_execution_indices or [])

    with open(json_path, "r", encoding="utf-8-sig") as f:
        data = json.load(f)

    X: List[List[float]] = []
    y_humidity: List[float] = []
    y_perlite_percent: List[float] = []
    y_phosphate_g: List[float] = []
    y_acidity_ml: List[float] = []
    groups: List[int] = []

    for exec_idx, execution in enumerate(data):
        if exec_idx in excluded:
            continue

        perlite_percent = perlite_percent_from_execution(execution)
        phosphate_applied_g, acidity_applied_ml = chemical_amounts_from_execution(execution)

        for barrido in execution.get("barridos", []):
            row: List[float] = []
            valid = True

            for feat in features:
                if feat not in barrido or barrido[feat] is None:
                    valid = False
                    break
                row.append(float(barrido[feat]))

            humidity = barrido.get("humedad")
            if humidity is None:
                valid = False

            if not valid:
                continue

            humidity_v = float(humidity)
            X.append(row)
            y_humidity.append(humidity_v)
            y_perlite_percent.append(perlite_percent)
            y_phosphate_g.append(phosphate_applied_g)
            y_acidity_ml.append(acidity_applied_ml)
            groups.append(exec_idx)

    return (
        np.array(X, dtype=np.float32),
        np.array(y_humidity, dtype=np.float32).reshape(-1, 1),
        np.array(y_perlite_percent, dtype=np.float32).reshape(-1, 1),
        np.array(y_phosphate_g, dtype=np.float32).reshape(-1, 1),
        np.array(y_acidity_ml, dtype=np.float32).reshape(-1, 1),
        np.array(groups, dtype=np.int32),
    )


# Crea una etiqueta por muestra para estratificar el split por ejecucion.
# Separa ejecuciones con perlita, con quimicos y muestras sin mezcla.
def build_split_stratification_labels(
    groups: np.ndarray,
    y_percent: np.ndarray,
    y_phosphate_g: np.ndarray,
    y_acidity_ml: np.ndarray,
) -> np.ndarray:
    split_label_by_group: Dict[int, int] = {}
    unique_groups = np.unique(groups)

    for group in unique_groups:
        mask = groups == group
        percent_value = float(y_percent[mask].reshape(-1)[0])
        phosphate_value = float(y_phosphate_g[mask].reshape(-1)[0])
        acidity_value = float(y_acidity_ml[mask].reshape(-1)[0])
        if abs(percent_value) > 1e-6:
            split_label_by_group[int(group)] = 100_000 + int(round(percent_value * 10.0))
        elif abs(phosphate_value) > 1e-6 or abs(acidity_value) > 1e-6:
            split_label_by_group[int(group)] = (
                200_000
                + int(round(phosphate_value * 1000.0)) * 1000
                + int(round(acidity_value * 10.0))
            )
        else:
            split_label_by_group[int(group)] = 300_000

    return np.array([split_label_by_group[int(group)] for group in groups], dtype=np.int32)


# Elige una fraccion de ejecuciones de cada clase para test o validacion.
def _take_class_split(groups_by_class: Dict[int, List[int]], fraction: float, rng: np.random.Generator) -> set[int]:
    selected: set[int] = set()
    for class_groups in groups_by_class.values():
        shuffled = np.array(class_groups, dtype=np.int32)
        rng.shuffle(shuffled)
        n_groups = len(shuffled)
        n_take = max(1, int(round(n_groups * fraction)))
        n_take = min(n_take, max(1, n_groups - 1))
        selected.update(int(v) for v in shuffled[:n_take])
    return selected


# Divide train/val/test manteniendo juntos todos los barridos de una misma ejecucion.
def split_by_execution_stratified(
    groups: np.ndarray,
    group_class_labels: np.ndarray,
    test_size: float,
    val_size: float,
    random_seed: int,
) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    unique_groups = np.unique(groups)
    if len(unique_groups) < 4:
        raise ValueError("Se necesitan al menos 4 ejecuciones para train/val/test")

    labels_by_group = []
    for group in unique_groups:
        labels = group_class_labels[groups == group].reshape(-1)
        labels_by_group.append(int(labels[0]))
    labels_by_group_arr = np.array(labels_by_group, dtype=np.int32)

    _, class_counts = np.unique(labels_by_group_arr, return_counts=True)
    if np.any(class_counts < 3):
        raise ValueError(
            "Cada estrato necesita al menos 3 ejecuciones para hacer split estratificado "
            f"por grupos. Conteo: {class_counts.tolist()}"
        )

    rng = np.random.default_rng(random_seed)
    groups_by_class: Dict[int, List[int]] = {int(c): [] for c in np.unique(labels_by_group_arr)}
    for group, label in zip(unique_groups, labels_by_group_arr):
        groups_by_class[int(label)].append(int(group))

    test_groups = _take_class_split(groups_by_class, test_size, rng)

    train_val_groups_by_class: Dict[int, List[int]] = {int(c): [] for c in np.unique(labels_by_group_arr)}
    for group, label in zip(unique_groups, labels_by_group_arr):
        if int(group) not in test_groups:
            train_val_groups_by_class[int(label)].append(int(group))

    relative_val_size = val_size / (1.0 - test_size)
    val_groups = _take_class_split(train_val_groups_by_class, relative_val_size, rng)
    train_groups = set(int(v) for v in unique_groups) - test_groups - val_groups

    train_idx = np.array([i for i, g in enumerate(groups) if int(g) in train_groups], dtype=np.int32)
    val_idx = np.array([i for i, g in enumerate(groups) if int(g) in val_groups], dtype=np.int32)
    test_idx = np.array([i for i, g in enumerate(groups) if int(g) in test_groups], dtype=np.int32)
    return train_idx, val_idx, test_idx


# Calcula media y desviacion usando solo el conjunto train.
def compute_norm_stats(values: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    mean = values.mean(axis=0)
    std = values.std(axis=0)
    std[std == 0] = 1.0
    return mean, std


# Escala los datos para que todas las variables tengan magnitudes comparables.
def normalize(values: np.ndarray, mean: np.ndarray, std: np.ndarray) -> np.ndarray:
    return (values - mean) / std


# Devuelve valores normalizados a la escala real.
def denormalize(values: np.ndarray, mean: np.ndarray, std: np.ndarray) -> np.ndarray:
    return values * std + mean


# Construye y compila la red neuronal que predice humedad.
def build_humidity_model(input_dim: int) -> tf.keras.Model:
    regularizer = tf.keras.regularizers.l2(1e-4)

    inputs = tf.keras.layers.Input(shape=(input_dim,), name="features")
    x = tf.keras.layers.Dense(48, activation="relu", kernel_regularizer=regularizer)(inputs)
    x = tf.keras.layers.Dropout(0.15)(x)
    x = tf.keras.layers.Dense(24, activation="relu", kernel_regularizer=regularizer)(x)
    x = tf.keras.layers.Dense(12, activation="relu", kernel_regularizer=regularizer)(x)
    outputs = tf.keras.layers.Dense(1, activation="linear", name="humidity")(x)

    model = tf.keras.Model(inputs=inputs, outputs=outputs, name="humidity_model")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=LEARNING_RATE),
        loss="mae",
        metrics=[tf.keras.metrics.MeanAbsoluteError(name="mae")],
    )

    return model


# Calcula MAE, RMSE y R2 para evaluar la humedad.
def regression_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    true = y_true.reshape(-1).astype(np.float32)
    pred = y_pred.reshape(-1).astype(np.float32)

    errors = pred - true
    abs_errors = np.abs(errors)
    sq_errors = np.square(errors)

    mae = float(np.mean(abs_errors))
    rmse = float(np.sqrt(np.mean(sq_errors)))

    ss_res = float(np.sum(sq_errors))
    ss_tot = float(np.sum(np.square(true - np.mean(true))))
    r2 = float(1.0 - ss_res / ss_tot) if ss_tot > 1e-12 else None

    return {
        "mae": mae,
        "rmse": rmse,
        "r2": r2,
    }


# Ejecuta todo el flujo: carga datos, divide, normaliza, entrena y evalua.
def main() -> None:
    tf.keras.backend.clear_session()
    tf.random.set_seed(RANDOM_SEED)
    np.random.seed(RANDOM_SEED)

    json_path = resolve_json_path()
    (
        X,
        y_humidity,
        y_percent,
        y_phosphate_g,
        y_acidity_ml,
        groups,
    ) = load_samples_from_json(
        json_path,
        FEATURES,
        excluded_execution_indices=EXCLUDED_EXECUTION_INDICES,
    )

    split_labels = build_split_stratification_labels(
        groups,
        y_percent,
        y_phosphate_g,
        y_acidity_ml,
    )
    train_idx, val_idx, test_idx = split_by_execution_stratified(
        groups,
        split_labels,
        test_size=TEST_SIZE,
        val_size=VAL_SIZE,
        random_seed=RANDOM_SEED,
    )

    X_train, X_val, X_test = X[train_idx], X[val_idx], X[test_idx]
    y_train, y_val, y_test = y_humidity[train_idx], y_humidity[val_idx], y_humidity[test_idx]

    
    x_mean, x_std = compute_norm_stats(X_train)
    y_mean, y_std = compute_norm_stats(y_train)

    X_train_n = normalize(X_train, x_mean, x_std)
    X_val_n = normalize(X_val, x_mean, x_std)
    X_test_n = normalize(X_test, x_mean, x_std)

    y_train_n = normalize(y_train, y_mean, y_std)
    y_val_n = normalize(y_val, y_mean, y_std)


    model = build_humidity_model(X_train_n.shape[1])
    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss",
            mode="min",
            patience=EARLY_STOPPING_PATIENCE,
            restore_best_weights=True,
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss",
            mode="min",
            factor=0.5,
            patience=LR_PATIENCE,
            min_lr=1e-5,
            verbose=0,
        ),
    ]

    history = model.fit(
        X_train_n,
        y_train_n,
        validation_data=(X_val_n, y_val_n),
        epochs=EPOCHS,
        batch_size=BATCH_SIZE,
        verbose=0,
        callbacks=callbacks,
    )

    y_pred_n = model.predict(X_test_n, verbose=0)
    y_pred = denormalize(y_pred_n, y_mean, y_std)
    metrics = regression_metrics(y_test, y_pred)

    print("Entrenamiento basico solo humedad")
    print(f"Dataset: {json_path}")
    print(f"Features: {FEATURES}")
    print(f"Muestras: train={len(train_idx)}, val={len(val_idx)}, test={len(test_idx)}")
    print(f"Epocas ejecutadas: {len(history.history['loss'])}")
    print()
    print("Metricas de test - humedad:")
    print(f"- MAE : {metrics['mae']:.4f}")
    print(f"- RMSE: {metrics['rmse']:.4f}")
    print(f"- R2  : {metrics['r2']:.4f}" if metrics["r2"] is not None else "- R2  : n/d")

if __name__ == "__main__":
    main()
