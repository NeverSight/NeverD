package fixture;

public final class Core {
    public Slot<?> saved;
    public Core() {}

    @SuppressWarnings("unchecked")
    private <E, S extends Slot<E>> S read(S input) {
        Trace.hit(1, this, input);
        saved = input;
        input.put((E)Trace.value);
        S result = Trace.replace ? (S)Trace.replacement : input;
        Trace.hit(2, this, input);
        return result;
    }

    public static Slot<?> call(Core subject, Slot<?> input) { return subject.read(input); }
    public static Object object(Core subject, Slot<?> input) { return subject.read(input); }
    public static void discard(Core subject, Slot<?> input) { subject.read(input); }
    @SuppressWarnings("unchecked")
    public static <E> Bag<E> checked(Core subject, Slot<E> input) {
        Slot<E> result = subject.read(input);
        Trace.hit(3, subject, input);
        return (Bag<E>)result;
    }
}
