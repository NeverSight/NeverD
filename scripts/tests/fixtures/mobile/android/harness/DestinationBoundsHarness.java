import fixture.Calls;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class DestinationBoundsHarness {
    static void check(boolean value) {
        if (!value) throw new AssertionError("destination transfer");
    }

    static final class FailingList extends ArrayList<Object> {
        final IOException failure = new IOException("write");
        @Override public boolean add(Object value) {
            if (size() == 1) DestinationBoundsHarness.<RuntimeException>raise(failure);
            return super.add(value);
        }
    }

    @SuppressWarnings("unchecked")
    static <E extends Throwable> void raise(Throwable value) throws E { throw (E) value; }

    public static void main(String[] args) throws IOException {
        String first = new String("first"), last = new String("last");
        Object[] mixed = {first, 2, null, last};
        List<Object> dest = new ArrayList<>();
        check(Calls.select(mixed, dest) == dest && dest.size() == 2);
        check(dest.get(0) == first && dest.get(1) == last);
        check(Calls.select(new Object[]{2, null}, null) == null);
        FailingList partial = new FailingList();
        try {
            Calls.select(mixed, partial);
            throw new AssertionError("missing checked failure");
        } catch (IOException expected) {
            check(expected == partial.failure && partial.size() == 1 && partial.get(0) == first);
        }
        java.util.function.Supplier<String> source = () -> first;
        String[] seen = new String[1];
        check(Calls.pick(source, value -> { seen[0] = value; return last; }) == source);
        check(seen[0] == first);
        RuntimeException failed = new IllegalStateException();
        try {
            Calls.pick(source, value -> { throw failed; });
            throw new AssertionError("missing callback failure");
        } catch (RuntimeException expected) {
            check(expected == failed);
        }
        String[] pair = {first, last};
        check(Calls.choose(Arrays.asList(first), pair) == 3);
        check(Calls.head(pair) == first);
        try { Calls.head(new String[0]); throw new AssertionError("empty input"); }
        catch (ArrayIndexOutOfBoundsException expected) { }
        Calls.exchange(pair, 0, 1);
        check(pair[0] == last && pair[1] == first);
        pair[1] = null;
        try {
            Calls.exchange(pair, 1, 0);
            throw new AssertionError("missing checked failure");
        } catch (IOException expected) {
            check("empty slot".equals(expected.getMessage()) && pair[0] == last && pair[1] == last);
        }
        dest.clear();
        check(Calls.append(new String[]{last, null}, dest) == dest);
        check(Calls.append(Arrays.asList(first, last), dest) == dest);
        check(Calls.append(new int[]{-7, 11}, dest) == dest);
        check(dest.equals(Arrays.asList(last, null, first, last, -7, 11)));
        CharSequence[] copied = Calls.range(new Object[]{2, first, last}, 1, 4);
        check(copied.getClass() == CharSequence[].class && copied.length == 3);
        check(copied[0] == first && copied[1] == last && copied[2] == null);
        try { Calls.range(mixed, 0, 2); throw new AssertionError("missing store check"); }
        catch (ArrayStoreException expected) { }
        try { Calls.range(mixed, 2, 1); throw new AssertionError("reversed endpoints"); }
        catch (IllegalArgumentException expected) { }
        System.out.println("destination effects and checked failure matched");
    }
}
