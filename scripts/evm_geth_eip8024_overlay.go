//===- evm_geth_eip8024_overlay.go - geth EIP-8024 oracle ------*- Go -*-===//
//
// NeverD Decompiler
//
// This source is never copied into the upstream checkout. The audit supplies it
// through go -overlay as a virtual core/vm package file, giving the probe a
// narrow, fail-closed bridge to the actual Amsterdam operation.execute fields.
//
//===----------------------------------------------------------------------===//

package vm

import (
	"errors"
	"fmt"
	"reflect"
	"runtime"
	"slices"

	"github.com/ethereum/go-ethereum/params"
)

const (
	neverDAuditErrorNone           = "none"
	neverDAuditErrorInvalidOpcode  = "invalid_opcode"
	neverDAuditErrorStackUnderflow = "stack_underflow"
	neverDAuditErrorNotRun         = "not_run"
	neverDAuditProgramCounterStart = uint64(0)
	neverDAuditImmediateBitWidth   = 8
	neverDAuditImmediateCount      = 1 << neverDAuditImmediateBitWidth
	neverDAuditStackMarkerBias     = 1
	neverDAuditChangedMarkerCount  = 2
	neverDAuditSingleFamily        = "single"
	neverDAuditPairFamily          = "pair"
	neverDAuditDupOperation        = "dup"
	neverDAuditSwapOperation       = "swap"
	neverDAuditExchangeOperation   = "exchange"
)

// NeverDAuditEIP8024Spec is the declarative opcode contract supplied by
// NeverD's EVMUpstreamSemanticsPolicy.def.
type NeverDAuditEIP8024Spec struct {
	Name            string `json:"name"`
	Byte            uint8  `json:"byte"`
	Family          string `json:"family"`
	OperationKind   string `json:"operation_kind"`
	ValidStackDelta int    `json:"valid_stack_delta"`
}

// NeverDAuditEIP8024Observation is one direct execution observation from an
// Amsterdam instruction handler. Operands are inferred from unique stack
// markers, never from a duplicate of geth's private decode formula.
type NeverDAuditEIP8024Observation struct {
	Opcode                       string `json:"opcode"`
	Encoded                      uint8  `json:"encoded"`
	Accepted                     bool   `json:"accepted"`
	Operands                     []int  `json:"operands"`
	ProgramCounterDelta          uint64 `json:"pc_delta"`
	ErrorClass                   string `json:"error_class"`
	StackDelta                   int    `json:"stack_delta"`
	MarkerTransitionVerified     bool   `json:"marker_transition_verified"`
	UnderflowErrorClass          string `json:"underflow_error_class"`
	UnderflowProgramCounterDelta uint64 `json:"underflow_pc_delta"`
	UnderflowStackUnchanged      bool   `json:"underflow_stack_unchanged"`
}

// NeverDAuditEIP8024MissingOperand records the PUSH-compatible missing-byte
// behavior separately from the complete 3 x 256 candidate matrix.
type NeverDAuditEIP8024MissingOperand struct {
	Opcode                   string `json:"opcode"`
	MatchesZeroImmediate     bool   `json:"matches_zero_immediate"`
	MarkerTransitionVerified bool   `json:"marker_transition_verified"`
}

// NeverDAuditEIP8024Handler identifies the concrete geth handler selected by a
// particular instruction table. This makes post-activation handler replacement
// visible even when the sampled behavior happens to remain unchanged.
type NeverDAuditEIP8024Handler struct {
	Opcode string `json:"opcode"`
	Symbol string `json:"symbol"`
}

// NeverDAuditEIP8024Table is the audit result for one concrete JumpTable.
type NeverDAuditEIP8024Table struct {
	Target         string                             `json:"target"`
	ActiveOpcodes  []string                           `json:"active_opcodes"`
	Handlers       []NeverDAuditEIP8024Handler        `json:"handlers"`
	Observations   []NeverDAuditEIP8024Observation    `json:"observations"`
	MissingOperand []NeverDAuditEIP8024MissingOperand `json:"missing_operand"`
}

type neverDAuditExecution struct {
	programCounterDelta uint64
	errorClass          string
	before              []uint64
	after               []uint64
}

func neverDAuditStack(size int) *Stack {
	stack := newStackForTesting()
	for index := 0; index < size; index++ {
		stack.get().SetUint64(uint64(index + neverDAuditStackMarkerBias))
	}
	return stack
}

func neverDAuditMarkers(stack *Stack) []uint64 {
	markers := make([]uint64, stack.len())
	for index, value := range stack.Data() {
		markers[index] = value.Uint64()
	}
	return markers
}

func neverDAuditErrorClass(err error) (string, error) {
	if err == nil {
		return neverDAuditErrorNone, nil
	}
	var invalid *ErrInvalidOpCode
	if errors.As(err, &invalid) {
		return neverDAuditErrorInvalidOpcode, nil
	}
	var underflow *ErrStackUnderflow
	if errors.As(err, &underflow) {
		return neverDAuditErrorStackUnderflow, nil
	}
	return "", fmt.Errorf("unexpected EIP-8024 execution error %T: %w", err, err)
}

func neverDAuditExecute(
	operation *operation,
	opcode OpCode,
	code []byte,
	stackSize int,
) (neverDAuditExecution, error) {
	if operation == nil || operation.execute == nil {
		return neverDAuditExecution{}, fmt.Errorf("%s has no executable operation", opcode)
	}
	stack := neverDAuditStack(stackSize)
	defer stack.release()
	before := neverDAuditMarkers(stack)
	pc := neverDAuditProgramCounterStart
	scope := &ScopeContext{Stack: stack, Contract: &Contract{Code: code}}
	_, executionError := operation.execute(&pc, nil, scope)
	errorClass, err := neverDAuditErrorClass(executionError)
	if err != nil {
		return neverDAuditExecution{}, fmt.Errorf("%s: %w", opcode, err)
	}
	return neverDAuditExecution{
		programCounterDelta: pc - neverDAuditProgramCounterStart,
		errorClass:          errorClass,
		before:              before,
		after:               neverDAuditMarkers(stack),
	}, nil
}

func neverDAuditChangedIndices(before, after []uint64) []int {
	if len(before) != len(after) {
		return nil
	}
	changed := make([]int, 0, neverDAuditChangedMarkerCount)
	for index := range before {
		if before[index] != after[index] {
			changed = append(changed, index)
		}
	}
	return changed
}

func neverDAuditOperands(
	kind string,
	before, after []uint64,
) ([]int, int, bool) {
	switch kind {
	case neverDAuditDupOperation:
		if len(after) != len(before)+1 || !slices.Equal(before, after[:len(before)]) {
			return nil, 0, false
		}
		for index, marker := range before {
			if marker == after[len(after)-1] {
				return []int{len(before) - index}, 1, true
			}
		}
		return nil, 0, false
	case neverDAuditSwapOperation:
		changed := neverDAuditChangedIndices(before, after)
		last := len(before) - 1
		if len(changed) != 2 || changed[1] != last ||
			after[changed[0]] != before[last] || after[last] != before[changed[0]] {
			return nil, 0, false
		}
		return []int{last - changed[0]}, 0, true
	case neverDAuditExchangeOperation:
		changed := neverDAuditChangedIndices(before, after)
		last := len(before) - 1
		if len(changed) != 2 || after[changed[0]] != before[changed[1]] ||
			after[changed[1]] != before[changed[0]] {
			return nil, 0, false
		}
		first := last - changed[1]
		second := last - changed[0]
		if first <= 0 || first >= second {
			return nil, 0, false
		}
		return []int{first, second}, 0, true
	default:
		return nil, 0, false
	}
}

func neverDAuditObservation(
	operation *operation,
	opcode OpCode,
	opcodeName string,
	kind string,
	encoded byte,
	code []byte,
) (NeverDAuditEIP8024Observation, error) {
	stackSize := int(params.StackLimit) - 1
	execution, err := neverDAuditExecute(operation, opcode, code, stackSize)
	if err != nil {
		return NeverDAuditEIP8024Observation{}, err
	}
	record := NeverDAuditEIP8024Observation{
		Opcode: opcodeName, Encoded: encoded,
		Operands:            []int{},
		ProgramCounterDelta: execution.programCounterDelta,
		ErrorClass:          execution.errorClass,
		StackDelta:          len(execution.after) - len(execution.before),
		UnderflowErrorClass: neverDAuditErrorNotRun,
	}
	if execution.errorClass == neverDAuditErrorInvalidOpcode {
		record.MarkerTransitionVerified = slices.Equal(execution.before, execution.after)
		return record, nil
	}
	if execution.errorClass != neverDAuditErrorNone {
		return NeverDAuditEIP8024Observation{}, fmt.Errorf(
			"%s immediate 0x%02x failed with %s on a sufficient stack",
			opcode, encoded, execution.errorClass)
	}
	operands, stackDelta, verified := neverDAuditOperands(
		kind, execution.before, execution.after)
	if !verified || stackDelta != record.StackDelta {
		return NeverDAuditEIP8024Observation{}, fmt.Errorf(
			"%s immediate 0x%02x has an unrecognized marker transition",
			opcode, encoded)
	}
	record.Accepted = true
	record.Operands = operands
	record.MarkerTransitionVerified = true
	required := operands[len(operands)-1]
	if kind != neverDAuditDupOperation {
		required++
	}
	underflow, err := neverDAuditExecute(operation, opcode, code, required-1)
	if err != nil {
		return NeverDAuditEIP8024Observation{}, err
	}
	record.UnderflowErrorClass = underflow.errorClass
	record.UnderflowProgramCounterDelta = underflow.programCounterDelta
	record.UnderflowStackUnchanged = slices.Equal(underflow.before, underflow.after)
	return record, nil
}

func neverDAuditValidSpec(spec NeverDAuditEIP8024Spec) bool {
	switch spec.OperationKind {
	case neverDAuditDupOperation, neverDAuditSwapOperation:
		return spec.Family == neverDAuditSingleFamily
	case neverDAuditExchangeOperation:
		return spec.Family == neverDAuditPairFamily
	default:
		return false
	}
}

func neverDAuditHandlerSymbol(operation *operation) (string, error) {
	if operation == nil || operation.execute == nil {
		return "", fmt.Errorf("operation has no executable handler")
	}
	function := runtime.FuncForPC(reflect.ValueOf(operation.execute).Pointer())
	if function == nil || function.Name() == "" {
		return "", fmt.Errorf("operation handler symbol is unavailable")
	}
	return function.Name(), nil
}

func neverDAuditSameExecution(left, right NeverDAuditEIP8024Observation) bool {
	return left.Accepted == right.Accepted &&
		slices.Equal(left.Operands, right.Operands) &&
		left.ProgramCounterDelta == right.ProgramCounterDelta &&
		left.ErrorClass == right.ErrorClass &&
		left.StackDelta == right.StackDelta &&
		left.MarkerTransitionVerified == right.MarkerTransitionVerified &&
		left.UnderflowErrorClass == right.UnderflowErrorClass &&
		left.UnderflowProgramCounterDelta == right.UnderflowProgramCounterDelta &&
		left.UnderflowStackUnchanged == right.UnderflowStackUnchanged
}

// NeverDAuditEIP8024 executes every candidate immediate through the operation
// functions in the caller-supplied table. It never selects a fork itself.
func NeverDAuditEIP8024(
	target string,
	table JumpTable,
	specs []NeverDAuditEIP8024Spec,
) (NeverDAuditEIP8024Table, error) {
	result := NeverDAuditEIP8024Table{
		Target:         target,
		ActiveOpcodes:  []string{},
		Handlers:       []NeverDAuditEIP8024Handler{},
		Observations:   []NeverDAuditEIP8024Observation{},
		MissingOperand: []NeverDAuditEIP8024MissingOperand{},
	}
	activeCount := 0
	for _, spec := range specs {
		if !neverDAuditValidSpec(spec) {
			return NeverDAuditEIP8024Table{}, fmt.Errorf(
				"%s has invalid family/operation contract for %s", target, spec.Name)
		}
		operation := table[spec.Byte]
		if operation == nil {
			return NeverDAuditEIP8024Table{}, fmt.Errorf(
				"%s byte 0x%02x has no operation metadata", target, spec.Byte)
		}
		if !operation.undefined {
			activeCount++
		}
	}
	if activeCount == 0 {
		return result, nil
	}
	if activeCount != len(specs) {
		return NeverDAuditEIP8024Table{}, fmt.Errorf(
			"%s partially activates the dynamic immediate opcode family", target)
	}
	result.Observations = make(
		[]NeverDAuditEIP8024Observation,
		0,
		len(specs)*neverDAuditImmediateCount,
	)
	result.MissingOperand = make(
		[]NeverDAuditEIP8024MissingOperand, 0, len(specs))
	for _, spec := range specs {
		opcode := OpCode(spec.Byte)
		if opcode.String() != spec.Name {
			return NeverDAuditEIP8024Table{}, fmt.Errorf(
				"%s byte 0x%02x runtime name is %q, expected %q",
				target, spec.Byte, opcode.String(), spec.Name)
		}
		operation := table[spec.Byte]
		handler, err := neverDAuditHandlerSymbol(operation)
		if err != nil {
			return NeverDAuditEIP8024Table{}, fmt.Errorf(
				"%s %s: %w", target, spec.Name, err)
		}
		result.ActiveOpcodes = append(result.ActiveOpcodes, spec.Name)
		result.Handlers = append(result.Handlers, NeverDAuditEIP8024Handler{
			Opcode: spec.Name, Symbol: handler,
		})
		var zero NeverDAuditEIP8024Observation
		for encoded := 0; encoded < neverDAuditImmediateCount; encoded++ {
			code := []byte{spec.Byte, byte(encoded)}
			record, err := neverDAuditObservation(
				operation, opcode, spec.Name, spec.OperationKind, byte(encoded), code)
			if err != nil {
				return NeverDAuditEIP8024Table{}, err
			}
			if record.Accepted && record.StackDelta != spec.ValidStackDelta {
				return NeverDAuditEIP8024Table{}, fmt.Errorf(
					"%s %s immediate 0x%02x stack delta is %d, expected %d",
					target, spec.Name, encoded, record.StackDelta,
					spec.ValidStackDelta)
			}
			if encoded == 0 {
				zero = record
			}
			result.Observations = append(result.Observations, record)
		}
		missingRecord, err := neverDAuditObservation(
			operation, opcode, spec.Name, spec.OperationKind, 0, []byte{spec.Byte})
		if err != nil {
			return NeverDAuditEIP8024Table{}, err
		}
		result.MissingOperand = append(
			result.MissingOperand, NeverDAuditEIP8024MissingOperand{
				Opcode:                   spec.Name,
				MatchesZeroImmediate:     neverDAuditSameExecution(missingRecord, zero),
				MarkerTransitionVerified: missingRecord.MarkerTransitionVerified,
			})
	}
	return result, nil
}
