# Domain Model — SyncBar Audio

---

## Entidades del Dominio

### 1. AudioDevice

Representa un dispositivo de salida de audio físico detectado por el sistema operativo.

| Atributo | Tipo | Descripción |
|----------|------|-------------|
| `uid` | `string` | Identificador único persistente del hardware (UID nativo de CoreAudio/WASAPI) |
| `native_name` | `string` | Nombre reportado por el sistema operativo |
| `custom_name` | `string?` | Nombre personalizado por el usuario (nullable) |
| `volume` | `float` | Nivel de volumen actual [0.0 – 1.0] |
| `is_muted` | `bool` | Estado de silencio individual del dispositivo |
| `is_active` | `bool` | `true` si el dispositivo está físicamente conectado |
| `is_connected` | `bool` | `true` si el SO reporta el dispositivo como disponible |

**Reglas de negocio:**
- El `uid` es inmutable y proviene del subsistema nativo (no se genera en la app)
- Si `custom_name` es null, la UI muestra `native_name`
- `is_active` = false cuando el dispositivo se desconecta; vuelve a true al reconectarse con el mismo UID
- `volume` se almacena en escala lineal; el núcleo convierte a dB para el HAL

---

### 2. VirtualGroup

Representa un agrupamiento lógico de hasta 4 dispositivos de audio creado por el usuario.

| Atributo | Tipo | Descripción |
|----------|------|-------------|
| `id` | `int` | Identificador único autoincremental |
| `name` | `string` | Nombre del grupo (editable por el usuario) |
| `master_volume` | `float` | Volumen maestro del grupo [0.0 – 1.0] |
| `is_muted` | `bool` | Estado de mute lógico del grupo (no propaga al hardware) |
| `is_active` | `bool` | `true` si al menos un dispositivo miembro está activo |

**Reglas de negocio:**
- Un grupo puede contener entre 1 y 4 `AudioDevice`
- El mute del grupo es **lógico**: no modifica `is_muted` de los dispositivos miembros
- Los grupos son **planos**: no pueden contener otros grupos
- Al aplicar `master_volume`, el núcleo calcula la ganancia proporcional de cada miembro
- Un grupo con todos sus miembros `is_active = false` se considera "sin señal"

---

### 3. GroupMembership

Tabla de relación entre `VirtualGroup` y `AudioDevice` (muchos a muchos).

| Atributo | Tipo | Descripción |
|----------|------|-------------|
| `group_id` | `int` | FK → VirtualGroup.id |
| `device_uid` | `string` | FK → AudioDevice.uid |
| `relative_volume` | `float` | Volumen relativo del dispositivo dentro del grupo [0.0 – 1.0] |
| `is_active` | `bool` | Refleja si el dispositivo está conectado en este contexto de grupo |

**Reglas de negocio:**
- `relative_volume` permite que cada miembro tenga un balance distinto dentro del grupo
- Al mover el slider maestro, se aplica: `device_volume = master_volume * relative_volume`
- La PK es compuesta: `(group_id, device_uid)`

---

## Diagrama de Relaciones

```
┌─────────────────────┐         ┌──────────────────────┐
│     AudioDevice     │         │    VirtualGroup       │
├─────────────────────┤         ├──────────────────────┤
│ uid (PK)            │         │ id (PK)              │
│ native_name         │         │ name                 │
│ custom_name         │         │ master_volume        │
│ volume              │         │ is_muted             │
│ is_muted            │         │ is_active            │
│ is_active           │         └──────────────────────┘
│ is_connected        │                   │
└─────────────────────┘                   │
           │                              │
           └──────────┬───────────────────┘
                      │
           ┌──────────────────────┐
           │   GroupMembership    │
           ├──────────────────────┤
           │ group_id (PK, FK)    │
           │ device_uid (PK, FK)  │
           │ relative_volume      │
           │ is_active            │
           └──────────────────────┘
```

---

## Servicios del Dominio (Core Engine)

### AudioDeviceService
- `detectDevices()` → Lista de `AudioDevice` activos
- `setVolume(uid, value: float)` → Aplica volumen al hardware vía HAL
- `setMute(uid, muted: bool)` → Aplica mute al hardware vía HAL
- `onDeviceConnected(uid)` → Callback reactivo del HAL
- `onDeviceDisconnected(uid)` → Marca `is_active = false`, persiste

### VirtualGroupService
- `createGroup(name)` → Crea grupo, persiste en SQLite
- `addDevice(groupId, uid)` → Valida límite de 4, agrega membresía
- `removeDevice(groupId, uid)` → Elimina membresía
- `setGroupVolume(groupId, value)` → Calcula proporciones y aplica a cada miembro activo
- `setGroupMute(groupId, muted)` → Mute lógico, no toca hardware
- `deleteGroup(groupId)` → Elimina grupo y membresías (CASCADE)

### PersistenceService
- `saveDeviceConfig(device)` → Upsert en tabla `devices` con debounce 300 ms
- `saveGroupConfig(group)` → Upsert en tabla `virtual_groups`
- `loadAll()` → Carga estado completo al iniciar la app
- `setDeviceLabel(uid, label)` → Actualiza `custom_name` inmediatamente

---

## Invariantes del Sistema

1. Un `AudioDevice` puede pertenecer a **múltiples grupos** simultáneamente
2. El volumen efectivo de un dispositivo en un grupo es `master_volume × relative_volume`
3. El mute de grupo **no** modifica el estado de mute del dispositivo en otros grupos
4. El UID de un dispositivo **nunca cambia** para el mismo hardware físico
5. La app **nunca** instala drivers, KEXTs ni modifica el kernel del SO
