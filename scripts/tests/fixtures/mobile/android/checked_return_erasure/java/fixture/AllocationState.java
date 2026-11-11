package fixture;

public final class AllocationState {
    public static Owner<?> owner;
    public static int initializations;
    public static int readsDuringInitialization;
    public static void initialize() {
        ++initializations;
        readsDuringInitialization = owner.reads;
        owner.stamp = Long.MAX_VALUE;
    }
}
