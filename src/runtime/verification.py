"""Exact WCE verification through a Yosys miter and ABC SAT."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .aiger import AigerError, read_aiger, write_structural_verilog
from .process import ProcessError, prepare_work_directory, resolve_executable, run_process


@dataclass(frozen=True)
class VerificationResult:
    status: str
    detail: str


def _quote(path: Path) -> str:
    return '"' + str(path).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _miter(inputs: int, outputs: int, threshold: int, path: Path) -> None:
    path.write_text(
        f"""module miter_top(input [{inputs-1}:0] pi, output fail);
wire [{outputs-1}:0] original_value, approximate_value, difference;
original original_instance(.pi(pi), .po(original_value));
approximate approximate_instance(.pi(pi), .po(approximate_value));
assign difference = original_value > approximate_value ?
                    original_value - approximate_value : approximate_value - original_value;
assign fail = difference > {outputs}'d{threshold};
endmodule
""",
        encoding="ascii",
    )


def verify_wce(
    original: str | Path,
    approximate: str | Path,
    threshold: int,
    *,
    abc: str | Path = "abc",
    yosys: str | Path = "yosys",
    work_dir: str | Path | None = None,
    timeout: float = 300.0,
    exhaustive_threads: int = 16,
) -> VerificationResult:
    work, temporary = prepare_work_directory(work_dir, prefix="als_wce_")
    try:
        if threshold < 0:
            raise ValueError("threshold must be non-negative")
        original_circuit = read_aiger(original)
        approximate_circuit = read_aiger(approximate)
        if len(original_circuit.inputs) != len(approximate_circuit.inputs):
            raise AigerError("PI count mismatch")
        if len(original_circuit.outputs) != len(approximate_circuit.outputs):
            raise AigerError("PO count mismatch")
        if not original_circuit.inputs or not original_circuit.outputs:
            raise AigerError("verification requires at least one PI and one PO")
        maximum_error = (1 << len(original_circuit.outputs)) - 1
        if threshold >= maximum_error:
            status, detail = "PASS", f"threshold covers the complete {len(original_circuit.outputs)}-bit range"
            result = VerificationResult(status, detail)
            if temporary is not None:
                temporary.cleanup()
            return result
        exhaustive = Path(__file__).resolve().parents[2] / "build" / "meow-wce"
        if (len(original_circuit.inputs) <= 24
                and len(original_circuit.outputs) <= 63
                and exhaustive.is_file()):
            exact_result = run_process(
                [exhaustive, "--original", original, "--approximate", approximate,
                 "--threshold", str(threshold), "--threads", str(exhaustive_threads)], timeout=timeout, check=False,
            )
            try:
                exact = json.loads(exact_result.stdout.strip())
            except json.JSONDecodeError as exc:
                raise ProcessError(
                    "exhaustive WCE output is not valid JSON"
                ) from exc
            exact_status = exact.get("status")
            if exact_status == "PASS" and exact_result.returncode == 0:
                status = "PASS"
                detail = (f"exhaustively proved WCE={exact['wce']} <= {threshold} "
                          f"over {exact['patterns']} patterns")
            elif exact_status == "FAIL" and exact_result.returncode == 1:
                status = "FAIL"
                detail = (f"exhaustive WCE={exact['wce']} exceeds {threshold} "
                          f"over {exact['patterns']} patterns")
            else:
                raise ProcessError(
                    "exhaustive WCE verification failed"
                )
            result = VerificationResult(status, detail)
            if temporary is not None:
                temporary.cleanup()
            return result
        yosys_exe = resolve_executable(yosys)
        abc_exe = resolve_executable(abc)
        original_v = work / "original.v"
        approximate_v = work / "approximate.v"
        miter_v = work / "miter.v"
        miter_aig = work / "miter.aig"
        write_structural_verilog(original_circuit, "original", original_v)
        write_structural_verilog(approximate_circuit, "approximate", approximate_v)
        _miter(len(original_circuit.inputs), len(original_circuit.outputs), threshold, miter_v)
        yosys_script = (
            f"read_verilog {_quote(original_v)} {_quote(approximate_v)} {_quote(miter_v)}; "
            "hierarchy -top miter_top; proc; flatten; opt; techmap; opt; simplemap; opt; aigmap; opt; "
            f"write_aiger {_quote(miter_aig)}"
        )
        run_process([yosys_exe, "-p", yosys_script], timeout=timeout)
        abc_result = run_process(
            [abc_exe, "-c", f"read_aiger {miter_aig}; strash; sat"], timeout=timeout, check=False,
        )
        transcript = (abc_result.stdout + "\n" + abc_result.stderr).lower()
        if "unsat" in transcript:
            status, detail = "PASS", f"proved WCE <= {threshold}"
        elif "sat" in transcript:
            status, detail = "FAIL", f"counterexample exists for WCE > {threshold}"
        elif abc_result.returncode:
            raise ProcessError(f"ABC exited with {abc_result.returncode}: {abc_result.stderr}")
        else:
            raise ProcessError("ABC SAT result was not recognized")
    except (AigerError, OSError, ProcessError, ValueError) as exc:
        status, detail = "ERROR", str(exc)
    result = VerificationResult(status, detail)
    if temporary is not None:
        temporary.cleanup()
    return result
