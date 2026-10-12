# Database Schema — SyncBar Audio (SQLite)

**Motor:** SQLite 3.x  
**Archivo:** `~/.config/syncbar-audio/syncbar.db` (macOS: `~/Library/Application Support/SyncBarAudio/syncbar.db`)

---

## Configuración de la Conexión

```sql
PRAGMA journal_mode = WAL;       -- Write-Ahead Logging para mejor concurrencia
PRAGMA foreign_keys = ON;        -- Integridad referencial obligatoria (RNF-03)
PRAGMA synchronous = NORMAL;     -- Balance entre seguridad y rendimiento
PRAGMA cache_size = -8000;       -- 8 MB de caché en memoria
```

---

## DDL — Definición de Tablas

### Tabla: `devices`

Almacena la configuración persistida de cada dispositivo de audio detectado.

```sql
CREATE TABLE IF NOT EXISTS devices (
    device_uid      TEXT        NOT NULL,   -- UID persistente del hardware (CoreAudio/WASAPI)
    custom_name     TEXT,                   -- Nombre personalizado por el usuario (nullable)
    saved_volume    REAL        NOT NULL DEFAULT 1.0,  -- Último volumen guardado [0.0–1.0]
    is_muted        INTEGER     NOT NULL DEFAULT 0,    -- 0 = false, 1 = true
    is_active       INTEGER     NOT NULL DEFAULT 1,    -- 0 = desconectado, 1 = conectado

    CONSTRAINT pk_devices PRIMARY KEY (device_uid),
    CONSTRAINT chk_volume CHECK (saved_volume >= 0.0 AND saved_volume <= 1.0),
    CONSTRAINT chk_is_muted CHECK (is_muted IN (0, 1)),
    CONSTRAINT chk_is_active CHECK (is_active IN (0, 1))
);
```

**Notas:**
- `device_uid` es el UID nativo del SO, no un UUID generado por la app
- `custom_name = NULL` indica que se debe mostrar el nombre nativo del dispositivo
- `saved_volume` se actualiza con debounce de 300 ms tras soltar el slider (RF-03)

---

### Tabla: `virtual_groups`

Define los grupos virtuales creados por el usuario.

```sql
CREATE TABLE IF NOT EXISTS virtual_groups (
    id              INTEGER     NOT NULL,
    name            TEXT        NOT NULL DEFAULT 'Nuevo Grupo',
    master_volume   REAL        NOT NULL DEFAULT 1.0,  -- Volumen maestro del grupo [0.0–1.0]
    is_muted        INTEGER     NOT NULL DEFAULT 0,    -- Mute lógico del grupo
    is_active       INTEGER     NOT NULL DEFAULT 1,    -- 0 si todos los miembros están inactivos

    CONSTRAINT pk_virtual_groups PRIMARY KEY (id AUTOINCREMENT),
    CONSTRAINT chk_name_length CHECK (length(name) <= 64),
    CONSTRAINT chk_master_volume CHECK (master_volume >= 0.0 AND master_volume <= 1.0),
    CONSTRAINT chk_group_muted CHECK (is_muted IN (0, 1)),
    CONSTRAINT chk_group_active CHECK (is_active IN (0, 1))
);
```

---

### Tabla: `group_devices`

Tabla intermedia que mapea la relación muchos-a-muchos entre grupos y dispositivos.

```sql
CREATE TABLE IF NOT EXISTS group_devices (
    group_id        INTEGER     NOT NULL,
    device_uid      TEXT        NOT NULL,
    relative_volume REAL        NOT NULL DEFAULT 1.0,  -- Balance relativo dentro del grupo [0.0–1.0]
    is_active       INTEGER     NOT NULL DEFAULT 1,    -- Refleja si el dispositivo está conectado

    CONSTRAINT pk_group_devices PRIMARY KEY (group_id, device_uid),
    CONSTRAINT fk_group FOREIGN KEY (group_id)
        REFERENCES virtual_groups(id) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_device FOREIGN KEY (device_uid)
        REFERENCES devices(device_uid) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT chk_relative_volume CHECK (relative_volume >= 0.0 AND relative_volume <= 1.0),
    CONSTRAINT chk_member_active CHECK (is_active IN (0, 1))
);
```

**Notas:**
- `ON DELETE CASCADE` en ambas FKs previene registros huérfanos (RNF-03)
- `relative_volume` permite balance individual dentro del grupo
- Volumen efectivo del dispositivo = `virtual_groups.master_volume × group_devices.relative_volume`

---

## Índices

```sql
-- Búsqueda rápida de membresías por dispositivo (para actualizar is_active en desconexión)
CREATE INDEX IF NOT EXISTS idx_group_devices_device
    ON group_devices(device_uid);

-- Búsqueda rápida de membresías por grupo (para calcular volumen maestro)
CREATE INDEX IF NOT EXISTS idx_group_devices_group
    ON group_devices(group_id);
```

---

## Trigger: Límite de 4 dispositivos por grupo

```sql
CREATE TRIGGER IF NOT EXISTS trg_max_devices_per_group
BEFORE INSERT ON group_devices
BEGIN
    SELECT RAISE(ABORT, 'GROUP_DEVICE_LIMIT_EXCEEDED')
    WHERE (
        SELECT COUNT(*) FROM group_devices
        WHERE group_id = NEW.group_id
    ) >= 4;
END;
```

---

## Trigger: Actualizar is_active del grupo

```sql
-- Cuando un dispositivo miembro cambia su is_active, recalcular el is_active del grupo
CREATE TRIGGER IF NOT EXISTS trg_update_group_active
AFTER UPDATE OF is_active ON group_devices
BEGIN
    UPDATE virtual_groups
    SET is_active = (
        SELECT MAX(is_active) FROM group_devices
        WHERE group_id = NEW.group_id
    )
    WHERE id = NEW.group_id;
END;
```

---

## Queries de Uso Frecuente

### Cargar estado completo al iniciar

```sql
-- Todos los dispositivos
SELECT device_uid, custom_name, saved_volume, is_muted, is_active
FROM devices;

-- Todos los grupos con sus miembros
SELECT
    vg.id           AS group_id,
    vg.name         AS group_name,
    vg.master_volume,
    vg.is_muted     AS group_muted,
    vg.is_active    AS group_active,
    gd.device_uid,
    gd.relative_volume,
    gd.is_active    AS member_active,
    d.custom_name,
    d.saved_volume  AS device_volume,
    d.is_muted      AS device_muted
FROM virtual_groups vg
LEFT JOIN group_devices gd ON vg.id = gd.group_id
LEFT JOIN devices d ON gd.device_uid = d.device_uid
ORDER BY vg.id, gd.device_uid;
```

### Upsert de configuración de dispositivo (con debounce en capa C++)

```sql
INSERT INTO devices (device_uid, custom_name, saved_volume, is_muted, is_active)
VALUES (?, ?, ?, ?, ?)
ON CONFLICT(device_uid) DO UPDATE SET
    custom_name  = excluded.custom_name,
    saved_volume = excluded.saved_volume,
    is_muted     = excluded.is_muted,
    is_active    = excluded.is_active;
```

### Marcar dispositivo como inactivo al desconectarse

```sql
-- Actualiza el dispositivo
UPDATE devices SET is_active = 0 WHERE device_uid = ?;

-- Actualiza todas sus membresías activas
UPDATE group_devices SET is_active = 0 WHERE device_uid = ?;
-- El trigger trg_update_group_active se encarga del resto
```

---

## Diagrama Relacional

```
devices                    group_devices              virtual_groups
─────────────────          ──────────────────         ──────────────────────
device_uid (PK) ◄──────── device_uid (PK, FK)        id (PK) ◄──────────────
custom_name                group_id   (PK, FK) ──────►                       
saved_volume               relative_volume            name
is_muted                   is_active                  master_volume
is_active                                             is_muted
                                                      is_active
```
