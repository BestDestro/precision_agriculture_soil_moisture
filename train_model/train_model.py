#!/usr/bin/env python3
import math

import numpy as np
import tensorflow as tf

import train_json_model as base


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


def main() -> None:
    tf.keras.backend.clear_session()
    tf.random.set_seed(RANDOM_SEED)
    np.random.seed(RANDOM_SEED)

    json_path = base.resolve_json_path()
    (
        X,
        y_humidity,
        _y_presence,
        y_liters,
        y_phosphate_g,
        y_acidity_ml,
        groups,
        _row_info,
        _execution_summaries,
    ) = base.load_samples_from_json(
        json_path,
        FEATURES,
        excluded_execution_indices=base.EXCLUDED_EXECUTION_INDICES,
    )

    y_percent = base.perlite_percentage_from_liters(y_liters)


    split_labels = base.build_split_stratification_labels(
        groups,
        y_percent,
        y_phosphate_g,
        y_acidity_ml,
        y_humidity,
    )
    train_idx, val_idx, test_idx = base.split_by_execution_stratified(
        groups,
        split_labels,
        test_size=base.TEST_SIZE,
        val_size=base.VAL_SIZE,
        random_seed=RANDOM_SEED,
    )

    X_train, X_val, X_test = X[train_idx], X[val_idx], X[test_idx]
    y_train, y_val, y_test = y_humidity[train_idx], y_humidity[val_idx], y_humidity[test_idx]

    
    # La normalització es calcula només amb el conjunt d'entrenament
    x_mean, x_std = base.compute_x_norm_stats(X_train)
    y_mean, y_std = base.compute_y_norm_stats(y_train)

    # S'aplica la mateixa normalització a train, validació i test
    X_train_n = base.normalize(X_train, x_mean, x_std)
    X_val_n = base.normalize(X_val, x_mean, x_std)
    X_test_n = base.normalize(X_test, x_mean, x_std)

    y_train_n = base.normalize(y_train, y_mean, y_std)
    y_val_n = base.normalize(y_val, y_mean, y_std)


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
    y_pred = base.denormalize(y_pred_n, y_mean, y_std)
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
