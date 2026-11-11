import fixture.Mode;
import java.util.EnumSet;

public class EnumConstantsHarness {
    public static void main(String[] args) {
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            if (Mode.BASE.score(value) != value + 3 || Mode.CUSTOM.score(value) != value - 7) {
                throw new AssertionError("Enum constructor or constant-specific dispatch changed");
            }
            System.out.println(Mode.BASE.score(value) + ":" + Mode.CUSTOM.score(value));
        }
        Mode[] first = Mode.values();
        Mode[] second = Mode.values();
        first[0] = null;
        if (first == second || second[0] != Mode.BASE || second[1] != Mode.CUSTOM
                || Mode.valueOf("CUSTOM") != Mode.CUSTOM
                || !Mode.BASE.name().equals("BASE") || Mode.BASE.ordinal() != 0
                || !Mode.CUSTOM.name().equals("CUSTOM") || Mode.CUSTOM.ordinal() != 1
                || Mode.CUSTOM.compareTo(Mode.BASE) != 1
                || Mode.CUSTOM.getDeclaringClass() != Mode.class
                || EnumSet.allOf(Mode.class).size() != 2
                || !Mode.CUSTOM.getClass().isAnonymousClass()
                || Mode.CUSTOM.getClass().getEnclosingClass() != Mode.class) {
            throw new AssertionError("Enum identity, order, array isolation or reflection changed");
        }
        try {
            Mode.valueOf("missing");
            throw new AssertionError("Unknown enum name was accepted");
        } catch (IllegalArgumentException expected) {
            System.out.println("unknown-rejected");
        }
        try {
            Mode.valueOf(null);
            throw new AssertionError("Null enum name was accepted");
        } catch (NullPointerException expected) {
            System.out.println("null-rejected");
        }
        System.out.println("enum-contract:matched");
    }
}
