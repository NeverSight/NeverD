import fixture.Creator;
public final class AllocationFieldBeforeCaller {
    public static void make(Creator creator) { creator.make(); }
}
