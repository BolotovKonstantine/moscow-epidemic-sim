"""Формат расчётной таблицы `.mtab` версии 1 (docs/sim-core.md#расчётный-пакет).

Запись — детерминированная: одинаковые колонки дают одинаковые байты. Чтение проверяет структуру
так же, как загрузчик ядра (#37): магию, версии, имя, пакет, описания колонок, выравнивание,
порядок и границы колонок без переполнения, NaN и бесконечность во float-колонках. Смысл значений
(ссылки, коды, CSR) проверяет simtables.py по схеме таблицы.
"""

import re
import struct
from dataclasses import dataclass

import numpy as np

MAGIC = b"MESMTAB\0"
FORMAT_VERSION = 1
ALIGN = 64
HEADER_FIXED = 128
COLUMN_DESC = 64
MAX_COLUMNS = 256
MAX_ROWS = (1 << 31) - 1
NAME_BYTES = 32
PACKAGE_ID_BYTES = 32
PACKAGE_VERSION_BYTES = 16
DTYPES = {"u8": 1, "u16": 2, "u32": 3, "u64": 4, "i8": 5, "i16": 6, "i32": 7, "i64": 8, "f32": 9, "f64": 10}
DTYPE_NAMES = {code: name for name, code in DTYPES.items()}
NUMPY = {"u8": "<u1", "u16": "<u2", "u32": "<u4", "u64": "<u8", "i8": "<i1", "i16": "<i2", "i32": "<i4", "i64": "<i8", "f32": "<f4", "f64": "<f8"}
ITEM_SIZE = {name: np.dtype(code).itemsize for name, code in NUMPY.items()}
# Метки беззнаковых целых: максимум типа — «нет значения», максимум минус 1 — «неизвестно».
NONE = {name: int(np.iinfo(NUMPY[name]).max) for name in ("u8", "u16", "u32", "u64")}
UNKNOWN = {name: value - 1 for name, value in NONE.items()}
PACKAGE_ID = re.compile(r"[a-z0-9-]{1,31}")
PACKAGE_VERSION = re.compile(r"[0-9A-Za-z.+-]{1,15}")
TABLE_NAME = re.compile(r"[a-z0-9_]{1,31}")
COLUMN_NAME = re.compile(r"[a-z0-9_]{1,31}")


class TableError(ValueError):
    """Таблица не соответствует формату."""


@dataclass(frozen=True)
class Table:
    name: str
    schema_version: int
    rows: int
    package_id: str
    package_version: str
    columns: dict  # имя → (dtype, массив numpy только для чтения)


def pad(n: int) -> int:
    return (n + ALIGN - 1) // ALIGN * ALIGN


def _fixed(text: str, size: int, what: str) -> bytes:
    raw = text.encode("ascii")
    if len(raw) >= size:
        raise TableError(f"{what} {text!r} длиннее {size - 1} байт")
    return raw + b"\0" * (size - len(raw))


def _unfixed(raw: bytes, what: str) -> str:
    """ASCII с нулями в конце: после первого нуля — только нули, до него — печатные символы."""
    text, _, rest = raw.partition(b"\0")
    if rest.strip(b"\0") or not text or any(byte < 0x21 or byte > 0x7E for byte in text):
        raise TableError(f"{what}: поле не является ASCII-строкой с нулями в конце")
    return text.decode("ascii")


def check_package(package_id: str, package_version: str):
    if not isinstance(package_id, str) or not PACKAGE_ID.fullmatch(package_id):
        raise TableError(f"расчётные таблицы требуют package_id вида [a-z0-9-]{{1,31}}, получено {package_id!r}")
    if not isinstance(package_version, str) or not PACKAGE_VERSION.fullmatch(package_version):
        raise TableError(f"расчётные таблицы требуют package_version вида [0-9A-Za-z.+-]{{1,15}}, получено {package_version!r}")


def encode_table(name: str, schema_version: int, rows: int, package_id: str, package_version: str, columns) -> bytes:
    """columns — список (имя, dtype, значения). Значения приводятся к типу с проверкой диапазона."""
    check_package(package_id, package_version)
    if not TABLE_NAME.fullmatch(name):
        raise TableError(f"недопустимое имя таблицы {name!r}")
    if not 1 <= len(columns) <= MAX_COLUMNS:
        raise TableError(f"{name}: число колонок {len(columns)} вне 1–{MAX_COLUMNS}")
    if not 0 <= rows <= MAX_ROWS:
        raise TableError(f"{name}: число строк {rows} вне 0–{MAX_ROWS}")
    names = [column[0] for column in columns]
    if len(set(names)) != len(names):
        raise TableError(f"{name}: повторяющиеся имена колонок")
    header_size = pad(HEADER_FIXED + COLUMN_DESC * len(columns))
    offset = header_size
    descs, blobs = [], []
    for column_name, dtype, values in columns:
        if not COLUMN_NAME.fullmatch(column_name):
            raise TableError(f"{name}: недопустимое имя колонки {column_name!r}")
        data = _as_dtype(np.asarray(values), dtype, f"{name}.{column_name}").tobytes()
        count = len(data) // ITEM_SIZE[dtype]
        descs.append(_fixed(column_name, NAME_BYTES, "имя колонки") + struct.pack("<B7x", DTYPES[dtype]) + struct.pack("<QQQ", count, offset, len(data)))
        blobs.append((offset, data))
        offset = pad(offset + len(data))
    head = (MAGIC + struct.pack("<II", FORMAT_VERSION, header_size) + _fixed(name, NAME_BYTES, "имя таблицы")
            + struct.pack("<IIQ", schema_version, len(columns), rows)
            + _fixed(package_id, PACKAGE_ID_BYTES, "package_id") + _fixed(package_version, PACKAGE_VERSION_BYTES, "package_version") + b"\0" * 16)
    assert len(head) == HEADER_FIXED
    out = bytearray(offset)
    out[:HEADER_FIXED] = head
    out[HEADER_FIXED:HEADER_FIXED + COLUMN_DESC * len(descs)] = b"".join(descs)
    for start, data in blobs:
        out[start:start + len(data)] = data
    return bytes(out)


def _as_dtype(values: np.ndarray, dtype: str, what: str) -> np.ndarray:
    if values.ndim != 1:
        raise TableError(f"{what}: колонка должна быть одномерной")
    target = np.dtype(NUMPY[dtype])
    if dtype in ("f32", "f64"):
        result = values.astype(target)
        if not np.all(np.isfinite(result)):
            raise TableError(f"{what}: NaN или бесконечность")
        return result
    if values.size == 0:
        return values.astype(target)
    if values.dtype.kind == "f":
        if not np.all(np.isfinite(values)) or np.any(values != np.rint(values)):
            raise TableError(f"{what}: нецелые значения в целочисленной колонке")
    elif values.dtype.kind not in "iub":
        raise TableError(f"{what}: тип {values.dtype} не числовой")
    info = np.iinfo(target)
    low, high = values.min(), values.max()
    if int(low) < info.min or int(high) > info.max:
        raise TableError(f"{what}: значения {int(low)}…{int(high)} вне диапазона {dtype}")
    return values.astype(target)


def decode_table(data: bytes, what: str) -> Table:
    """Разобрать таблицу и проверить её структуру. Колонки — массивы поверх копии байтов."""
    size = len(data)
    if size < HEADER_FIXED:
        raise TableError(f"{what}: файл короче заголовка ({size} байт)")
    if data[:8] != MAGIC:
        raise TableError(f"{what}: неверная магия")
    version, header_size = struct.unpack_from("<II", data, 8)
    if version != FORMAT_VERSION:
        raise TableError(f"{what}: версия формата {version}, поддерживается {FORMAT_VERSION}")
    name = _unfixed(data[16:48], f"{what}: имя таблицы")
    schema_version, column_count, rows = struct.unpack_from("<IIQ", data, 48)
    package_id = _unfixed(data[64:96], f"{what}: package_id")
    package_version = _unfixed(data[96:112], f"{what}: package_version")
    if data[112:128].strip(b"\0"):
        raise TableError(f"{what}: резерв заголовка не нулевой")
    if not 1 <= column_count <= MAX_COLUMNS:
        raise TableError(f"{what}: число колонок {column_count} вне 1–{MAX_COLUMNS}")
    if rows > MAX_ROWS:
        raise TableError(f"{what}: число строк {rows} не меньше 2³¹")
    if header_size % ALIGN or header_size < HEADER_FIXED + COLUMN_DESC * column_count or header_size > size:
        raise TableError(f"{what}: длина заголовка {header_size} неверна")
    if data[HEADER_FIXED + COLUMN_DESC * column_count:header_size].strip(b"\0"):
        raise TableError(f"{what}: заполнение заголовка не нулевое")
    columns = {}
    previous_end = header_size
    buffer = bytes(data)
    for i in range(column_count):
        base = HEADER_FIXED + COLUMN_DESC * i
        column = _unfixed(data[base:base + NAME_BYTES], f"{what}: имя колонки {i}")
        code = data[base + 32]
        if data[base + 33:base + 40].strip(b"\0"):
            raise TableError(f"{what}.{column}: резерв описания не нулевой")
        count, offset, length = struct.unpack_from("<QQQ", data, base + 40)
        dtype = DTYPE_NAMES.get(code)
        if dtype is None:
            raise TableError(f"{what}.{column}: неизвестный тип {code}")
        if column in columns:
            raise TableError(f"{what}: колонка {column} повторяется")
        # Границы — вычитанием и делением, без сложения и умножения u64.
        if offset % ALIGN or offset < previous_end or offset > size or length > size - offset:
            raise TableError(f"{what}.{column}: колонка вне файла, не выровнена или перекрывает предыдущую")
        if length % ITEM_SIZE[dtype] or length // ITEM_SIZE[dtype] != count:
            raise TableError(f"{what}.{column}: размер {length} не равен {count} элементам {dtype}")
        values = np.frombuffer(buffer, dtype=NUMPY[dtype], count=count, offset=offset)
        if dtype in ("f32", "f64") and not np.all(np.isfinite(values)):
            raise TableError(f"{what}.{column}: NaN или бесконечность")
        columns[column] = (dtype, values)
        previous_end = offset + length
    return Table(name, schema_version, rows, package_id, package_version, columns)
