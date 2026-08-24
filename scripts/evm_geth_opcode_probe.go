//===- evm_geth_opcode_probe.go - Export geth EVM metadata as JSON -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"math/big"
	"os"
	"reflect"
	"runtime"
	"sort"
	"unicode/utf8"

	"github.com/ethereum/go-ethereum/core/vm"
	"github.com/ethereum/go-ethereum/params"
)

const (
	schemaVersion                 = 3
	opcodeBitWidth                = 8
	opcodeCount                   = 1 << opcodeBitWidth
	operationUndefinedField       = "undefined"
	mappedForkSelector            = "MappedForkSelector"
	noOpcodeAllocation            = "NoOpcodeAllocation"
	excludedSelectorExpectedError = "ExcludedSelectorExpectedError"
	maxProbeRequestBytes          = 1 << 20
	maxProbeStringBytes           = 256
	maxProbeRuleFields            = 128
	maxProbeRuleProbes            = 128
	maxProbeForks                 = 128
	maxProbeRulesPerFork          = 128
	maxProbeOpcodes               = opcodeCount
	maxProbeEIP8024Specs          = opcodeCount
	maxProbeEIP8024Observations   = 1 << 16
	maxProbeDiagnosticBytes       = 4096
	maxProbeDiagnosticSample      = 8
	mainnetActiveTarget           = "mainnet.active"
	mainnetScheduledTarget        = "mainnet.scheduled"
)

type forkRequest struct {
	Name  string   `json:"name"`
	Rules []string `json:"rules"`
}

type ruleProbeRequest struct {
	Name         string `json:"name"`
	Category     string `json:"category"`
	ExpectedFork string `json:"expected_fork"`
}

type opcodeRequest struct {
	Name                  string `json:"name"`
	Byte                  uint8  `json:"byte"`
	ActiveWithoutCostFrom *int   `json:"active_without_cost_from,omitempty"`
}

type probeRequest struct {
	SchemaVersion int                         `json:"schema_version"`
	Authority     string                      `json:"authority"`
	GethRemote    string                      `json:"geth_remote"`
	GethRef       string                      `json:"geth_ref"`
	GethRevision  string                      `json:"geth_revision"`
	AuditUnixTime uint64                      `json:"audit_unix_time"`
	RuleFields    []string                    `json:"rule_fields"`
	RuleProbes    []ruleProbeRequest          `json:"rule_probes"`
	Forks         []forkRequest               `json:"forks"`
	Opcodes       []opcodeRequest             `json:"opcodes"`
	EIP8024Specs  []vm.NeverDAuditEIP8024Spec `json:"eip8024_specs"`
}

type opcodeRecord struct {
	Name          string `json:"name"`
	Byte          uint8  `json:"byte"`
	BaseMinStack  int    `json:"base_min_stack"`
	NetStackDelta int    `json:"net_stack_delta"`
}

type forkRecord struct {
	Name    string         `json:"name"`
	Rules   []string       `json:"rules"`
	Opcodes []opcodeRecord `json:"opcodes"`
}

type ruleProbeRecord struct {
	Name         string         `json:"name"`
	Category     string         `json:"category"`
	ExpectedFork string         `json:"expected_fork"`
	LookupError  bool           `json:"lookup_error"`
	Opcodes      []opcodeRecord `json:"opcodes"`
}

type mainnetForkRecord struct {
	UpstreamFork string         `json:"upstream_fork"`
	Rules        []string       `json:"rules"`
	Opcodes      []opcodeRecord `json:"opcodes"`
}

type mainnetRecord struct {
	Active    mainnetForkRecord `json:"active"`
	Scheduled mainnetForkRecord `json:"scheduled"`
}

type eip8024Record struct {
	Tables []vm.NeverDAuditEIP8024Table `json:"tables"`
}

type probeResponse struct {
	SchemaVersion int               `json:"schema_version"`
	Authority     string            `json:"authority"`
	GethRemote    string            `json:"geth_remote"`
	GethRef       string            `json:"geth_ref"`
	GethRevision  string            `json:"geth_revision"`
	AuditUnixTime uint64            `json:"audit_unix_time"`
	GoVersion     string            `json:"go_version"`
	StackLimit    int               `json:"stack_limit"`
	Forks         []forkRecord      `json:"forks"`
	RuleProbes    []ruleProbeRecord `json:"rule_probes"`
	Mainnet       mainnetRecord     `json:"mainnet"`
	EIP8024       eip8024Record     `json:"eip8024"`
}

func rulesFromNames(context string, names []string) (params.Rules, error) {
	rules := params.Rules{}
	value := reflect.ValueOf(&rules).Elem()
	for _, name := range names {
		field := value.FieldByName(name)
		if !field.IsValid() || !field.CanSet() || field.Kind() != reflect.Bool {
			return params.Rules{}, fmt.Errorf(
				"%s names unavailable geth rule %q", context, name)
		}
		field.SetBool(true)
	}
	return rules, nil
}

func instructionSet(request forkRequest) (vm.JumpTable, error) {
	rules, err := rulesFromNames("fork "+request.Name, request.Rules)
	if err != nil {
		return vm.JumpTable{}, err
	}
	return vm.LookupInstructionSet(rules)
}

func exportedBooleanRuleFields() ([]string, error) {
	typeOfRules := reflect.TypeOf(params.Rules{})
	fields := make([]string, 0, typeOfRules.NumField())
	for index := 0; index < typeOfRules.NumField(); index++ {
		field := typeOfRules.Field(index)
		if field.PkgPath != "" {
			continue
		}
		if field.Type.Kind() != reflect.Bool {
			return nil, fmt.Errorf(
				"params.Rules has exported non-boolean field %s of kind %s",
				field.Name, field.Type.Kind())
		}
		fields = append(fields, field.Name)
	}
	sort.Strings(fields)
	return fields, nil
}

func enabledRuleFields(rules params.Rules) ([]string, error) {
	value := reflect.ValueOf(rules)
	typeOfRules := value.Type()
	fields := make([]string, 0, typeOfRules.NumField())
	for index := 0; index < typeOfRules.NumField(); index++ {
		field := typeOfRules.Field(index)
		if field.PkgPath != "" {
			continue
		}
		if field.Type.Kind() != reflect.Bool {
			return nil, fmt.Errorf(
				"params.Rules has exported non-boolean field %s of kind %s",
				field.Name, field.Type.Kind())
		}
		if value.Field(index).Bool() {
			fields = append(fields, field.Name)
		}
	}
	sort.Strings(fields)
	return fields, nil
}

func equalStrings(left, right []string) bool {
	if len(left) != len(right) {
		return false
	}
	for index := range left {
		if left[index] != right[index] {
			return false
		}
	}
	return true
}

func validateString(context, value string) error {
	if value == "" {
		return fmt.Errorf("%s must not be empty", context)
	}
	if len(value) > maxProbeStringBytes {
		return fmt.Errorf(
			"%s exceeds the %d-byte limit", context, maxProbeStringBytes)
	}
	return nil
}

func validateRequestShape(request probeRequest) error {
	for context, value := range map[string]string{
		"authority": request.Authority, "geth remote": request.GethRemote,
		"geth ref": request.GethRef, "geth revision": request.GethRevision,
	} {
		if err := validateString(context, value); err != nil {
			return err
		}
	}
	if len(request.RuleFields) > maxProbeRuleFields {
		return fmt.Errorf("rule field count exceeds %d", maxProbeRuleFields)
	}
	if len(request.RuleProbes) > maxProbeRuleProbes {
		return fmt.Errorf("rule probe count exceeds %d", maxProbeRuleProbes)
	}
	if len(request.Forks) == 0 || len(request.Forks) > maxProbeForks {
		return fmt.Errorf("fork count must be between 1 and %d", maxProbeForks)
	}
	if len(request.Opcodes) > maxProbeOpcodes {
		return fmt.Errorf("opcode count exceeds %d", maxProbeOpcodes)
	}
	if len(request.EIP8024Specs) > maxProbeEIP8024Specs {
		return fmt.Errorf("EIP-8024 spec count exceeds %d", maxProbeEIP8024Specs)
	}
	for index, name := range request.RuleFields {
		if err := validateString(fmt.Sprintf("rule field %d", index), name); err != nil {
			return err
		}
	}
	for index, probe := range request.RuleProbes {
		for context, value := range map[string]string{
			"name": probe.Name, "category": probe.Category,
			"expected fork": probe.ExpectedFork,
		} {
			if err := validateString(
				fmt.Sprintf("rule probe %d %s", index, context), value); err != nil {
				return err
			}
		}
	}
	for index, fork := range request.Forks {
		if err := validateString(fmt.Sprintf("fork %d name", index), fork.Name); err != nil {
			return err
		}
		if len(fork.Rules) > maxProbeRulesPerFork {
			return fmt.Errorf(
				"fork %s rule count exceeds %d", fork.Name, maxProbeRulesPerFork)
		}
		seenRules := make(map[string]struct{}, len(fork.Rules))
		for ruleIndex, rule := range fork.Rules {
			if err := validateString(
				fmt.Sprintf("fork %s rule %d", fork.Name, ruleIndex), rule); err != nil {
				return err
			}
			if _, exists := seenRules[rule]; exists {
				return fmt.Errorf("fork %s repeats enabled rule %s", fork.Name, rule)
			}
			seenRules[rule] = struct{}{}
		}
	}
	for index, opcode := range request.Opcodes {
		if err := validateString(
			fmt.Sprintf("opcode %d name", index), opcode.Name); err != nil {
			return err
		}
		if opcode.ActiveWithoutCostFrom != nil &&
			(*opcode.ActiveWithoutCostFrom < 0 ||
				*opcode.ActiveWithoutCostFrom >= len(request.Forks)) {
			return fmt.Errorf(
				"opcode %s zero-cost activation index is outside the fork inventory",
				opcode.Name)
		}
	}
	for index, spec := range request.EIP8024Specs {
		for context, value := range map[string]string{
			"name": spec.Name, "family": spec.Family,
			"operation kind": spec.OperationKind,
		} {
			if err := validateString(
				fmt.Sprintf("EIP-8024 spec %d %s", index, context), value); err != nil {
				return err
			}
		}
		if spec.ValidStackDelta < -opcodeCount ||
			spec.ValidStackDelta > opcodeCount {
			return fmt.Errorf(
				"EIP-8024 spec %s stack delta is outside the bounded range", spec.Name)
		}
	}
	return nil
}

func stringSample(values []string) []string {
	limit := len(values)
	if limit > maxProbeDiagnosticSample {
		limit = maxProbeDiagnosticSample
	}
	return values[:limit]
}

func operationIsUndefined(operation any) (bool, error) {
	value := reflect.ValueOf(operation)
	if value.Kind() != reflect.Pointer || value.IsNil() {
		return false, fmt.Errorf("geth jump table contains a non-operation entry")
	}
	value = value.Elem()
	if value.Kind() != reflect.Struct {
		return false, fmt.Errorf("geth jump table operation is not a struct")
	}
	undefined := value.FieldByName(operationUndefinedField)
	if !undefined.IsValid() || undefined.Kind() != reflect.Bool {
		return false, fmt.Errorf(
			"geth operation.undefined metadata is unavailable or not boolean")
	}
	return undefined.Bool(), nil
}

func chargeEIP8024Table(
	target string,
	table vm.JumpTable,
	specs []vm.NeverDAuditEIP8024Spec,
	used *int,
) error {
	activeCount := 0
	for _, spec := range specs {
		undefined, err := operationIsUndefined(table[spec.Byte])
		if err != nil {
			return fmt.Errorf("%s EIP-8024 byte 0x%02x: %w", target, spec.Byte, err)
		}
		if !undefined {
			activeCount++
		}
	}
	if activeCount == 0 {
		return nil
	}
	if activeCount != len(specs) {
		return fmt.Errorf(
			"%s partially activates the dynamic immediate opcode family", target)
	}
	observations := len(specs) * opcodeCount
	if observations > maxProbeEIP8024Observations-*used {
		return fmt.Errorf(
			"EIP-8024 observation budget exceeds %d at %s",
			maxProbeEIP8024Observations, target)
	}
	*used += observations
	return nil
}

func inspectInstructionSet(
	label string,
	table vm.JumpTable,
	forkIndex int,
	knownBytes map[uint8]opcodeRequest,
	opcodes []opcodeRequest,
) ([]opcodeRecord, error) {
	for opcodeByte := 0; opcodeByte < opcodeCount; opcodeByte++ {
		encoded := uint8(opcodeByte)
		operation := table[encoded]
		undefined, err := operationIsUndefined(operation)
		if err != nil {
			return nil, fmt.Errorf("%s byte 0x%02x: %w", label, encoded, err)
		}
		hasCost := operation.HasCost()
		if undefined && hasCost {
			return nil, fmt.Errorf(
				"%s byte 0x%02x is undefined but reports a cost", label, encoded)
		}
		active := !undefined
		opcode, known := knownBytes[encoded]
		if active && !known {
			return nil, fmt.Errorf(
				"%s activates unreviewed opcode byte 0x%02x", label, encoded)
		}
		expectedWithoutCost := active && known &&
			opcode.ActiveWithoutCostFrom != nil &&
			forkIndex >= *opcode.ActiveWithoutCostFrom
		actualWithoutCost := active && !hasCost
		if actualWithoutCost != expectedWithoutCost {
			return nil, fmt.Errorf(
				"%s zero-cost activation policy mismatch at byte 0x%02x",
				label, encoded)
		}
	}
	records := make([]opcodeRecord, 0, len(opcodes))
	for _, opcode := range opcodes {
		operation := table[opcode.Byte]
		undefined, err := operationIsUndefined(operation)
		if err != nil {
			return nil, fmt.Errorf("%s byte 0x%02x: %w", label, opcode.Byte, err)
		}
		if undefined {
			continue
		}
		minimum, maximum := operation.Stack()
		records = append(records, opcodeRecord{
			Name: opcode.Name, Byte: opcode.Byte, BaseMinStack: minimum,
			NetStackDelta: int(params.StackLimit) - maximum,
		})
	}
	return records, nil
}

func validateRuleProbeRequests(
	requests []ruleProbeRequest,
	ruleFields []string,
	forkIndices map[string]int,
) error {
	names := make([]string, 0, len(requests))
	seen := make(map[string]struct{}, len(requests))
	for _, request := range requests {
		if _, exists := seen[request.Name]; exists {
			return fmt.Errorf("probe request repeats rule probe %s", request.Name)
		}
		seen[request.Name] = struct{}{}
		names = append(names, request.Name)
		switch request.Category {
		case mappedForkSelector, noOpcodeAllocation, excludedSelectorExpectedError:
		default:
			return fmt.Errorf(
				"probe request gives rule %s unknown category %s",
				request.Name, request.Category)
		}
		if _, exists := forkIndices[request.ExpectedFork]; !exists {
			return fmt.Errorf(
				"probe request gives rule %s unknown expected fork %s",
				request.Name, request.ExpectedFork)
		}
	}
	sort.Strings(names)
	if !equalStrings(names, ruleFields) {
		return fmt.Errorf(
			"rule probe inventory drift: expected %d %v, got %d %v",
			len(ruleFields), stringSample(ruleFields), len(names), stringSample(names))
	}
	return nil
}

func probeMainnetFork(
	label string,
	timestamp uint64,
	forkIndex int,
	knownBytes map[uint8]opcodeRequest,
	opcodes []opcodeRequest,
) (mainnetForkRecord, vm.JumpTable, error) {
	maximumBlock := new(big.Int).SetUint64(^uint64(0))
	rules := params.MainnetChainConfig.Rules(maximumBlock, true, timestamp)
	table, err := vm.LookupInstructionSet(rules)
	if err != nil {
		return mainnetForkRecord{}, vm.JumpTable{}, fmt.Errorf(
			"%s instruction set: %w", label, err)
	}
	records, err := inspectInstructionSet(
		label, table, forkIndex, knownBytes, opcodes)
	if err != nil {
		return mainnetForkRecord{}, vm.JumpTable{}, err
	}
	ruleNames, err := enabledRuleFields(rules)
	if err != nil {
		return mainnetForkRecord{}, vm.JumpTable{}, err
	}
	return mainnetForkRecord{
		UpstreamFork: params.MainnetChainConfig.LatestFork(timestamp).String(),
		Rules:        ruleNames,
		Opcodes:      records,
	}, table, nil
}

func probe(request probeRequest) (probeResponse, error) {
	if err := validateRequestShape(request); err != nil {
		return probeResponse{}, fmt.Errorf("invalid probe request: %w", err)
	}
	if request.SchemaVersion != schemaVersion {
		return probeResponse{}, fmt.Errorf(
			"unsupported probe request schema version %d", request.SchemaVersion)
	}
	if request.AuditUnixTime == 0 {
		return probeResponse{}, fmt.Errorf("probe request has no audit Unix time")
	}
	actualRuleFields, err := exportedBooleanRuleFields()
	if err != nil {
		return probeResponse{}, err
	}
	expectedRuleFields := append([]string{}, request.RuleFields...)
	sort.Strings(expectedRuleFields)
	for index := 1; index < len(expectedRuleFields); index++ {
		if expectedRuleFields[index-1] == expectedRuleFields[index] {
			return probeResponse{}, fmt.Errorf(
				"probe request repeats params.Rules field %s",
				expectedRuleFields[index])
		}
	}
	if !equalStrings(actualRuleFields, expectedRuleFields) {
		return probeResponse{}, fmt.Errorf(
			"params.Rules boolean field inventory drift: expected %d %v, got %d %v",
			len(expectedRuleFields), stringSample(expectedRuleFields),
			len(actualRuleFields), stringSample(actualRuleFields))
	}
	forkIndices := make(map[string]int, len(request.Forks))
	for index, fork := range request.Forks {
		if _, exists := forkIndices[fork.Name]; exists {
			return probeResponse{}, fmt.Errorf("probe request repeats fork %s", fork.Name)
		}
		forkIndices[fork.Name] = index
	}
	if err := validateRuleProbeRequests(
		request.RuleProbes, expectedRuleFields, forkIndices); err != nil {
		return probeResponse{}, err
	}
	knownBytes := make(map[uint8]opcodeRequest, len(request.Opcodes))
	knownNames := make(map[string]struct{}, len(request.Opcodes))
	for _, opcode := range request.Opcodes {
		if _, exists := knownNames[opcode.Name]; exists {
			return probeResponse{}, fmt.Errorf(
				"probe request repeats opcode name %s", opcode.Name)
		}
		knownNames[opcode.Name] = struct{}{}
		if previous, exists := knownBytes[opcode.Byte]; exists {
			return probeResponse{}, fmt.Errorf(
				"probe request assigns byte 0x%02x to both %s and %s",
				opcode.Byte, previous.Name, opcode.Name)
		}
		knownBytes[opcode.Byte] = opcode
	}
	seenEIP8024Names := make(map[string]struct{}, len(request.EIP8024Specs))
	seenEIP8024Bytes := make(map[uint8]struct{}, len(request.EIP8024Specs))
	for _, spec := range request.EIP8024Specs {
		if _, exists := seenEIP8024Names[spec.Name]; exists {
			return probeResponse{}, fmt.Errorf(
				"probe request repeats EIP-8024 spec %s", spec.Name)
		}
		if _, exists := seenEIP8024Bytes[spec.Byte]; exists {
			return probeResponse{}, fmt.Errorf(
				"probe request repeats EIP-8024 byte 0x%02x", spec.Byte)
		}
		opcode, exists := knownBytes[spec.Byte]
		if !exists || opcode.Name != spec.Name {
			return probeResponse{}, fmt.Errorf(
				"EIP-8024 spec %s byte 0x%02x is not an exact opcode request",
				spec.Name, spec.Byte)
		}
		seenEIP8024Names[spec.Name] = struct{}{}
		seenEIP8024Bytes[spec.Byte] = struct{}{}
	}
	response := probeResponse{
		SchemaVersion: schemaVersion,
		Authority:     request.Authority,
		GethRemote:    request.GethRemote,
		GethRef:       request.GethRef,
		GethRevision:  request.GethRevision,
		AuditUnixTime: request.AuditUnixTime,
		GoVersion:     runtime.Version(),
		StackLimit:    int(params.StackLimit),
		Forks:         make([]forkRecord, 0, len(request.Forks)),
		RuleProbes:    make([]ruleProbeRecord, 0, len(request.RuleProbes)),
		EIP8024: eip8024Record{Tables: make(
			[]vm.NeverDAuditEIP8024Table, 0, len(request.Forks)+2)},
	}
	activeCounts := make([]int, len(request.Opcodes))
	canonicalTables := make([]vm.JumpTable, 0, len(request.Forks))
	for forkIndex, fork := range request.Forks {
		table, err := instructionSet(fork)
		if err != nil {
			return probeResponse{}, err
		}
		records, err := inspectInstructionSet(
			"fork "+fork.Name, table, forkIndex, knownBytes, request.Opcodes)
		if err != nil {
			return probeResponse{}, err
		}
		for opcodeIndex, opcode := range request.Opcodes {
			undefined, err := operationIsUndefined(table[opcode.Byte])
			if err != nil {
				return probeResponse{}, err
			}
			if !undefined {
				activeCounts[opcodeIndex]++
			}
		}
		response.Forks = append(response.Forks, forkRecord{
			Name: fork.Name, Rules: append([]string{}, fork.Rules...), Opcodes: records,
		})
		canonicalTables = append(canonicalTables, table)
	}
	for opcodeIndex, count := range activeCounts {
		if count == 0 {
			return probeResponse{}, fmt.Errorf(
				"opcode %s is inactive in every requested fork",
				request.Opcodes[opcodeIndex].Name)
		}
	}
	for _, ruleProbe := range request.RuleProbes {
		rules, err := rulesFromNames("rule probe "+ruleProbe.Name, []string{ruleProbe.Name})
		if err != nil {
			return probeResponse{}, err
		}
		table, lookupError := vm.LookupInstructionSet(rules)
		records, err := inspectInstructionSet(
			"rule probe "+ruleProbe.Name, table,
			forkIndices[ruleProbe.ExpectedFork], knownBytes, request.Opcodes)
		if err != nil {
			return probeResponse{}, err
		}
		response.RuleProbes = append(response.RuleProbes, ruleProbeRecord{
			Name: ruleProbe.Name, Category: ruleProbe.Category,
			ExpectedFork: ruleProbe.ExpectedFork,
			LookupError:  lookupError != nil, Opcodes: records,
		})
	}
	active, activeTable, err := probeMainnetFork(
		"active mainnet", request.AuditUnixTime, len(request.Forks)-1,
		knownBytes, request.Opcodes)
	if err != nil {
		return probeResponse{}, err
	}
	scheduled, scheduledTable, err := probeMainnetFork(
		"latest scheduled mainnet", ^uint64(0), len(request.Forks)-1,
		knownBytes, request.Opcodes)
	if err != nil {
		return probeResponse{}, err
	}
	response.Mainnet = mainnetRecord{Active: active, Scheduled: scheduled}
	eip8024Targets := make([]struct {
		name  string
		table vm.JumpTable
	}, 0, len(request.Forks)+2)
	for index, fork := range request.Forks {
		eip8024Targets = append(eip8024Targets, struct {
			name  string
			table vm.JumpTable
		}{name: fork.Name, table: canonicalTables[index]})
	}
	eip8024Targets = append(eip8024Targets, []struct {
		name  string
		table vm.JumpTable
	}{
		{name: mainnetActiveTarget, table: activeTable},
		{name: mainnetScheduledTarget, table: scheduledTable},
	}...)
	eip8024ObservationCount := 0
	for _, target := range eip8024Targets {
		if err := chargeEIP8024Table(
			target.name, target.table, request.EIP8024Specs,
			&eip8024ObservationCount); err != nil {
			return probeResponse{}, fmt.Errorf("probe EIP-8024 immediates: %w", err)
		}
		tableRecord, err := vm.NeverDAuditEIP8024(
			target.name, target.table, request.EIP8024Specs)
		if err != nil {
			return probeResponse{}, fmt.Errorf("probe EIP-8024 immediates: %w", err)
		}
		response.EIP8024.Tables = append(response.EIP8024.Tables, tableRecord)
	}
	return response, nil
}

func fatalf(context string, err error) {
	diagnostic := fmt.Sprintf("%s: %v", context, err)
	contentLimit := maxProbeDiagnosticBytes - 1
	if len(diagnostic) > contentLimit {
		digest := sha256.Sum256([]byte(diagnostic))
		suffix := fmt.Sprintf(" [truncated sha256=%x]", digest)
		prefixLimit := contentLimit - len(suffix)
		for prefixLimit > 0 && !utf8.ValidString(diagnostic[:prefixLimit]) {
			prefixLimit--
		}
		diagnostic = diagnostic[:prefixLimit] + suffix
	}
	fmt.Fprintln(os.Stderr, diagnostic)
	os.Exit(1)
}

func main() {
	limitedInput := io.LimitReader(os.Stdin, maxProbeRequestBytes+1)
	encoded, err := io.ReadAll(limitedInput)
	if err != nil {
		fatalf("read probe request", err)
	}
	if len(encoded) > maxProbeRequestBytes {
		fatalf("read probe request", fmt.Errorf(
			"input exceeds the %d-byte limit", maxProbeRequestBytes))
	}
	if !utf8.Valid(encoded) {
		fatalf("decode probe request", fmt.Errorf("input is not valid UTF-8"))
	}
	var request probeRequest
	decoder := json.NewDecoder(bytes.NewReader(encoded))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		fatalf("decode probe request", err)
	}
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		if err == nil {
			err = fmt.Errorf("multiple JSON values")
		}
		fatalf("decode probe request trailing data", err)
	}
	response, err := probe(request)
	if err != nil {
		fatalf("probe geth instruction sets", err)
	}
	if err := json.NewEncoder(os.Stdout).Encode(response); err != nil {
		fatalf("encode probe response", err)
	}
}
