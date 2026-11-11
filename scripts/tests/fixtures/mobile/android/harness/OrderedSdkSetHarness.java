import fixture.OrderedBag;
import java.lang.reflect.*;
import java.util.*;

public final class OrderedSdkSetHarness {
    private static int values;
    private static int identities;
    private static int failures;
    private static final RuntimeException SENTINEL = new RuntimeException("owned comparator failure");
    private static final class Order implements Comparator<Integer> {
        final boolean reverse;
        int calls;
        Order(boolean reverse) { this.reverse = reverse; }
        public int compare(Integer a, Integer b) {
            calls++;
            if (a != null && a == 999 || b != null && b == 999) throw SENTINEL;
            int result = a == b ? 0 : a == null ? -1 : b == null ? 1 : Integer.compare(a, b);
            return reverse ? -result : result;
        }
    }
    private static void check(boolean condition) { if (!condition) throw new AssertionError(); }
    private static void same(OrderedBag<Integer> actual, TreeSet<Integer> expected, Order order, Method size, Method getter) throws Exception {
        int calls = order.calls;
        for (int result : new int[]{actual.size(), actual.getSize(), ((Set<Integer>) actual).size(),
                (Integer) size.invoke(actual), (Integer) getter.invoke(actual)}) {
            check(result == expected.size()); values++;
        }
        check(order.calls == calls);
        Object[] a = actual.toArray(), e = expected.toArray();
        check(a.length == e.length);
        for (int i = 0; i < a.length; i++) { check(a[i] == e[i]); identities++; }
        Iterator<Integer> ai = actual.descendingIterator(), ei = expected.descendingIterator();
        while (ei.hasNext()) { check(ai.hasNext() && ai.next() == ei.next()); identities++; }
        check(!ai.hasNext());
        check(actual.comparator() == order); identities++;
    }
    public static void main(String[] args) throws Exception {
        boolean original = args[0].equals("original");
        Method size = OrderedBag.class.getDeclaredMethod("size"), getter = OrderedBag.class.getDeclaredMethod("getSize");
        check(size.isBridge() == original && getter.isBridge() == original);
        check(!size.isSynthetic() && !getter.isSynthetic() && Modifier.isFinal(size.getModifiers()));
        check(!Modifier.isFinal(getter.getModifiers()) && Modifier.isFinal(OrderedBag.class.getModifiers()));
        check(OrderedBag.class.getGenericSuperclass().getTypeName().equals("java.util.TreeSet<E>"));
        Type parameter = OrderedBag.class.getDeclaredConstructors()[0].getGenericParameterTypes()[0];
        check(parameter.getTypeName().equals("java.util.Comparator<? super E>"));
        Integer[] tokens = new Integer[]{null, new Integer(Integer.MIN_VALUE), new Integer(-3),
                new Integer(0), new Integer(11), new Integer(Integer.MAX_VALUE)};
        for (boolean reverse : new boolean[]{false, true}) {
            Order order = new Order(reverse), control = new Order(reverse);
            OrderedBag<Integer> bag = new OrderedBag<>(order);
            TreeSet<Integer> reference = new TreeSet<>(control);
            for (int step = 0; step < 64; step++) {
                Integer token = tokens[(step * 5 + 1) % tokens.length];
                switch (step % 8) {
                    case 0: case 1: case 2: check(bag.add(token) == reference.add(token)); break;
                    case 3: check(bag.contains(token) == reference.contains(token)); break;
                    case 4: check(bag.remove(token) == reference.remove(token)); break;
                    case 5: check(bag.pollFirst() == reference.pollFirst()); identities++; break;
                    case 6: check(bag.pollLast() == reference.pollLast()); identities++; break;
                    default: bag.addAll(Arrays.asList(tokens)); reference.addAll(Arrays.asList(tokens));
                }
                check(order.calls == control.calls);
                same(bag, reference, order, size, getter);
            }
            SequencedCollection<Integer>[] views = new SequencedCollection[]{bag.reversed(),
                    ((SortedSet<Integer>) bag).reversed(),
                    ((SequencedSet<Integer>) bag).reversed(),
                    ((SequencedCollection<Integer>) bag).reversed()};
            for (SequencedCollection<Integer> view : views) {
                check(view instanceof NavigableSet);
                Object[] a = view.toArray(), e = reference.descendingSet().toArray();
                check(a.length == e.length);
                for (int i = 0; i < a.length; i++) { check(a[i] == e[i]); identities++; }
            }
            NavigableSet<Integer> descending = bag.descendingSet();
            check(descending.pollFirst() == reference.descendingSet().pollFirst()); identities++;
            same(bag, reference, order, size, getter);
            int before = bag.size();
            try { bag.add(999); throw new AssertionError(); }
            catch (RuntimeException error) { check(error == SENTINEL && bag.size() == before); failures++; }
            bag.clear(); reference.clear();
            same(bag, reference, order, size, getter);
            try { bag.first(); throw new AssertionError(); }
            catch (NoSuchElementException expected) { failures++; }
            try { bag.last(); throw new AssertionError(); }
            catch (NoSuchElementException expected) { failures++; }
        }
        OrderedBag<Integer> natural = new OrderedBag<>(null);
        check(natural.comparator() == null); identities++;
        try { natural.add(null); throw new AssertionError(); }
        catch (NullPointerException expected) { failures++; }
        check(values == 660 && failures == 7);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("ordered-sdk-state-and-comparator-effects=matched,full-superclass-contract-and-original-defaults=retained");
    }
}
