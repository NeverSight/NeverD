import fixture.EntryTable;
import fixture.Mode;
import java.util.EnumSet;

public class EnumArrayHarness {
    public static void main(String[] args) {
        EntryTable table = Mode.entries();
        if (table != Mode.entries() || table.size() != 2
                || table.get(-1) != null || table.get(2) != null
                || table.get(0) != Mode.FIRST || table.get(1) != Mode.SECOND) {
            throw new AssertionError("Enum initializer or stored-array reads changed");
        }
        Mode[] copy = Mode.values();
        copy[0] = null;
        if (Mode.values()[0] != Mode.FIRST || table.get(0) != Mode.FIRST
                || EnumSet.allOf(Mode.class).size() != 2) {
            throw new AssertionError("Enum values array isolation changed");
        }
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            if (table.get(0).score(value) != value + 3
                    || table.get(1).score(value) != value + 7) {
                throw new AssertionError("Stored enum constant behavior changed");
            }
            System.out.println(table.get(0).score(value) + ":" + table.get(1).score(value));
        }
        try {
            new EntryTable(null).size();
            throw new AssertionError("Null array was accepted");
        } catch (NullPointerException expected) {
            System.out.println("null-rejected");
        }
        System.out.println("enum-array-contract:matched");
    }
}
