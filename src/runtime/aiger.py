"""Small, dependency-free AIGER reader and structural utilities."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO


class AigerError(RuntimeError):
    """Raised when a combinational AIGER file is malformed or unsupported."""


@dataclass(frozen=True)
class AigerCircuit:
    maximum_variable: int
    inputs: tuple[int, ...]
    outputs: tuple[int, ...]
    ands: tuple[tuple[int, int, int], ...]

    @property
    def gate_ids(self) -> tuple[int, ...]:
        return tuple(lhs >> 1 for lhs, _, _ in self.ands)


def _header(stream: BinaryIO, path: Path) -> tuple[str, int, int, int, int]:
    raw = stream.readline()
    try:
        fields = raw.decode("ascii").split()
        if len(fields) != 6 or fields[0] not in {"aag", "aig"}:
            raise ValueError
        maximum, inputs, latches, outputs, ands = map(int, fields[1:])
    except (UnicodeDecodeError, ValueError) as exc:
        raise AigerError(f"{path}: invalid AIGER header") from exc
    if min(maximum, inputs, latches, outputs, ands) < 0:
        raise AigerError(f"{path}: negative AIGER count")
    if latches:
        raise AigerError(f"{path}: sequential AIGER is unsupported")
    if fields[0] == "aig" and maximum != inputs + ands:
        raise AigerError(f"{path}: binary AIGER must use canonical variable numbering")
    return fields[0], maximum, inputs, outputs, ands


def _varint(stream: BinaryIO, path: Path) -> int:
    value = shift = 0
    while shift < 70:
        raw = stream.read(1)
        if not raw:
            raise AigerError(f"{path}: truncated binary AND section")
        byte = raw[0]
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value
        shift += 7
    raise AigerError(f"{path}: oversized binary delta")


def _integer(stream: BinaryIO, path: Path, section: str) -> int:
    raw = stream.readline()
    try:
        return int(raw.strip())
    except ValueError as exc:
        raise AigerError(f"{path}: malformed {section}") from exc


def _validate(circuit: AigerCircuit, path: Path) -> None:
    defined = {0}
    for literal in circuit.inputs:
        variable = literal >> 1
        if literal <= 0 or literal & 1 or variable in defined or variable > circuit.maximum_variable:
            raise AigerError(f"{path}: invalid PI literal {literal}")
        defined.add(variable)
    previous = 0
    for lhs, rhs0, rhs1 in circuit.ands:
        variable = lhs >> 1
        if lhs <= previous or lhs & 1 or variable in defined or variable > circuit.maximum_variable:
            raise AigerError(f"{path}: invalid AND lhs {lhs}")
        if any(rhs < 0 or (rhs >> 1) not in defined for rhs in (rhs0, rhs1)):
            raise AigerError(f"{path}: AND {lhs} references an undefined literal")
        previous = lhs
        defined.add(variable)
    for literal in circuit.outputs:
        if literal < 0 or (literal >> 1) not in defined:
            raise AigerError(f"{path}: invalid PO literal {literal}")


def read_aiger(filename: str | Path) -> AigerCircuit:
    """Read an ASCII or binary combinational AIGER circuit."""
    path = Path(filename).resolve()
    try:
        stream = path.open("rb")
    except OSError as exc:
        raise AigerError(f"cannot open {path}: {exc}") from exc
    with stream:
        fmt, maximum, input_count, output_count, and_count = _header(stream, path)
        if fmt == "aag":
            inputs = tuple(_integer(stream, path, "PI literal") for _ in range(input_count))
            outputs = tuple(_integer(stream, path, "PO literal") for _ in range(output_count))
            rows: list[tuple[int, int, int]] = []
            for _ in range(and_count):
                try:
                    row = tuple(map(int, stream.readline().split()))
                except ValueError as exc:
                    raise AigerError(f"{path}: malformed ASCII AND") from exc
                if len(row) != 3:
                    raise AigerError(f"{path}: malformed ASCII AND")
                rows.append(row)
            ands = tuple(rows)
        else:
            inputs = tuple(2 * (index + 1) for index in range(input_count))
            outputs = tuple(_integer(stream, path, "binary PO literal") for _ in range(output_count))
            rows = []
            for index in range(and_count):
                lhs = 2 * (input_count + index + 1)
                rhs0 = lhs - _varint(stream, path)
                rhs1 = rhs0 - _varint(stream, path)
                rows.append((lhs, rhs0, rhs1))
            ands = tuple(rows)
    circuit = AigerCircuit(maximum, inputs, outputs, ands)
    _validate(circuit, path)
    return circuit


def _verilog_literal(literal: int) -> str:
    expression = f"n[{literal >> 1}]"
    return f"~{expression}" if literal & 1 else expression


def write_structural_verilog(circuit: AigerCircuit, module: str, filename: str | Path) -> None:
    """Emit plain structural Verilog suitable for Yosys normalization."""
    if not circuit.inputs or not circuit.outputs:
        raise AigerError("verification requires at least one PI and one PO")
    lines = [
        f"module {module}(input [{len(circuit.inputs)-1}:0] pi, output [{len(circuit.outputs)-1}:0] po);",
        f"wire [{circuit.maximum_variable}:0] n;",
        "assign n[0] = 1'b0;",
    ]
    lines += [f"assign n[{lit >> 1}] = pi[{index}];" for index, lit in enumerate(circuit.inputs)]
    lines += [
        f"assign n[{lhs >> 1}] = {_verilog_literal(rhs0)} & {_verilog_literal(rhs1)};"
        for lhs, rhs0, rhs1 in circuit.ands
    ]
    lines += [f"assign po[{index}] = {_verilog_literal(lit)};" for index, lit in enumerate(circuit.outputs)]
    lines.append("endmodule")
    Path(filename).write_text("\n".join(lines) + "\n", encoding="ascii")
