import fixture.Cell;
import fixture.Provider;
import fixture.Slot;
import java.lang.reflect.ParameterizedType;

public class AnonymousBaseHarness {
    public static void main(String[] args) {
        Slot<String> slot = Provider.get();
        if (slot != Provider.get() || slot.initial != 7) {
            throw new AssertionError("Superclass constructor or shared identity changed");
        }
        for (int value : new int[]{Integer.MIN_VALUE, -3, 0, 7, Integer.MAX_VALUE}) {
            if (slot.bump(value) != value + 1) {
                throw new AssertionError("Inherited concrete method changed");
            }
            System.out.println(slot.bump(value));
        }
        Cell cell = new Cell("first");
        Slot raw = slot;
        System.out.println(raw.read(cell));
        raw.write(cell, "second");
        System.out.println(slot.read(cell));
        try {
            raw.write(cell, Integer.valueOf(3));
            throw new AssertionError("Bridge cast was lost");
        } catch (ClassCastException expected) {
            if (!cell.text.equals("second")) throw new AssertionError("Cast ran after the write");
            System.out.println("cast-rejected");
        }
        raw.write(cell, null);
        if (slot.read(cell) != null) throw new AssertionError("Bridge changed null");
        Class<?> type = slot.getClass();
        if (type.getEnclosingClass() != Provider.class
                || type.getEnclosingMethod() != null
                || ((ParameterizedType) type.getGenericSuperclass()).getActualTypeArguments()[0] != String.class) {
            throw new AssertionError("Superclass type binding or enclosure changed");
        }
        int bridges = 0;
        for (java.lang.reflect.Method method : type.getDeclaredMethods()) {
            if (method.isBridge()) {
                if (!method.isSynthetic()) throw new AssertionError("Bridge is not synthetic");
                ++bridges;
            }
        }
        System.out.println("anonymous=" + type.isAnonymousClass()
                + ",member=" + type.isMemberClass() + ",bridges=" + bridges);
    }
}
