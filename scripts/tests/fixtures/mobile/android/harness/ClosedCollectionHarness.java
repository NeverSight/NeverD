import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.TypeVariable;
import java.lang.reflect.WildcardType;
import java.util.ArrayList;
import java.util.Collection;
import java.util.LinkedList;

@SuppressWarnings({"rawtypes", "unchecked"})
public final class ClosedCollectionHarness {
    static void check(boolean condition) { if (!condition) throw new AssertionError("owned closed collection bound changed"); }
    static final class Text implements CharSequence {
        char[] units;
        int visible, lengths, reads;
        Text(char[] units, int visible) { this.units = units; this.visible = visible; }
        public int length() {
            lengths++; Trace.hit(3);
            if (Trace.mode == 18 && lengths == 2) throw Trace.failure;
            return visible;
        }
        public char charAt(int index) {
            reads++; Trace.hit(4);
            if (Trace.mode == 20 && reads == 2) throw Trace.error;
            return units[index];
        }
        public CharSequence subSequence(int begin, int end) { throw new AssertionError("unexpected subsequence"); }
        public String toString() { throw new AssertionError("unexpected text conversion"); }
    }
    static final class Destination extends ArrayList<Object> {
        final Text original, late;
        final Collection other;
        int calls;
        Destination(Text original, Text late, Collection other) { this.original = original; this.late = late; this.other = other; }
        public boolean add(Object value) {
            check(value == Trace.box && value instanceof Character); calls++; Trace.hit(5);
            if (Trace.mode == 21 && calls == 2) throw Trace.failure;
            super.add(value);
            Trace.text = late; Trace.destination = other; Trace.box = Character.valueOf('x');
            if (calls == 1) {
                if (Trace.mode == 1) original.visible = 1;
                if (Trace.mode == 2) original.visible = 5;
                if (Trace.mode == 3) original.units[1] = '\uffff';
                if (Trace.mode == 4) original.units = null;
            }
            return false;
        }
    }
    static void metadata() throws Exception {
        Method method = Provider.class.getDeclaredMethod("collect", CharSequence.class, Collection.class);
        check(Modifier.isPublic(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) && !method.isSynthetic() && !method.isVarArgs());
        check(method.getReturnType() == Collection.class && method.getParameterCount() == 2 && method.getExceptionTypes().length == 0);
        TypeVariable<?>[] formals = method.getTypeParameters(); check(formals.length == 1);
        TypeVariable<?> formal = formals[0]; check(formal.getGenericDeclaration().equals(method) && formal.getBounds().length == 1);
        check(formal.getBounds()[0] instanceof ParameterizedType);
        ParameterizedType bound = (ParameterizedType)formal.getBounds()[0];
        check(bound.getRawType() == Collection.class && bound.getActualTypeArguments().length == 1 && bound.getActualTypeArguments()[0] instanceof WildcardType);
        WildcardType element = (WildcardType)bound.getActualTypeArguments()[0];
        check(element.getLowerBounds().length == 1 && element.getLowerBounds()[0] == Character.class && element.getUpperBounds().length == 1 && element.getUpperBounds()[0] == Object.class);
        Type[] parameters = method.getGenericParameterTypes();
        check(parameters[0] == CharSequence.class && parameters[1].equals(formal) && method.getGenericReturnType().equals(formal));
    }
    static Object invoke(int route, int operation) throws Throwable {
        if (route == 0) return operation == 0 ? Client.call() : Client.checked();
        if (route == 1) return operation == 0 ? ClosedCollectionCaller.call() : ClosedCollectionCaller.checked();
        String name = operation == 0 ? "call" : "checked";
        if (route == 2) {
            try { return Client.class.getMethod(name).invoke(null); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        if (operation == 0) return (Object)MethodHandles.lookup().findStatic(Client.class, name, MethodType.methodType(Object.class)).invokeExact();
        return (ArrayList)MethodHandles.lookup().findStatic(Client.class, name, MethodType.methodType(ArrayList.class)).invokeExact();
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), operation = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]); metadata();
        for (int attempt = 0; attempt < 2; attempt++) {
            char[] units = mode == 24 ? new char[]{0, 127, 'C', 'D', 'E'} : new char[]{'A', '\ud800', '\udc00', '\uffff', 0};
            int initialLength = mode == 2 ? 1 : mode == 7 ? 0 : mode == 17 ? 5 : mode == 19 ? 6 : mode == 24 ? 2 : 3;
            Text text = new Text(units, initialLength), late = new Text(new char[]{'z'}, 1);
            Collection other = new ArrayList(); Destination destination = new Destination(text, late, other);
            Trace.text = mode == 5 ? null : text; Trace.destination = mode == 6 || mode == 7 ? null : destination;
            Trace.returned = mode == 16 ? null : new LinkedList();
            Object sentinel = new Object(); Trace.after = sentinel; Trace.box = null; Trace.mode = mode;
            Trace.failure = new IllegalArgumentException("original"); Trace.error = new LinkageError("original");
            Trace.fail = mode >= 8 && mode <= 14 ? mode - 7 : mode == 22 ? 8 : mode == 23 ? -5 : 0;
            Trace.events = ""; Throwable failure = null; Object actual = null;
            try { actual = invoke(route, operation); } catch (Throwable caught) { failure = caught; }
            String expectedEvents = "9,"; if (attempt == 0) expectedEvents += "1,";
            int queries = 0, reads = 0, calls = 0, length = initialLength;
            ArrayList<Character> expected = new ArrayList<>();
            int failureKind = 0; boolean finished = false, poisoned = false;
            if (mode == 8) failureKind = 5;
            else {
                expectedEvents += "2,";
                if (mode == 9) failureKind = 1;
                else if (mode == 5) failureKind = 3;
                else for (int index = 0; ; index++) {
                    queries++; expectedEvents += "3,";
                    if (mode == 10 || mode == 18 && queries == 2) { failureKind = 1; break; }
                    if (index >= length) { expectedEvents += "7,"; if (mode == 14) failureKind = 1; else finished = true; break; }
                    reads++; expectedEvents += "4,";
                    if (mode == 11) { failureKind = 1; break; }
                    if (mode == 20 && reads == 2) { failureKind = 2; break; }
                    if (poisoned) { failureKind = 3; break; }
                    if (index >= 5) { failureKind = 4; break; }
                    char value = mode == 3 && index == 1 ? '\uffff' : units[index];
                    if (mode == 6) { failureKind = 3; break; }
                    calls++; expectedEvents += "5,";
                    if (mode == 12 || mode == 21 && calls == 2) { failureKind = 1; break; }
                    if (mode == 23) { failureKind = 2; break; }
                    expected.add(Character.valueOf(value));
                    if (calls == 1) {
                        if (mode == 1) length = 1;
                        if (mode == 2) length = 5;
                        if (mode == 4) poisoned = true;
                    }
                    expectedEvents += "6,";
                    if (mode == 13) { failureKind = 1; break; }
                }
            }
            Object result = mode == 15 || mode == 16 ? Trace.returned : mode == 6 || mode == 7 ? null : destination;
            if (finished) { expectedEvents += "8,"; if (mode == 22) failureKind = 1; else if (operation == 1 && mode == 15) failureKind = 6; }
            if (mode == 8 && attempt == 1) expectedEvents = "9,";
            check(Trace.events.equals(expectedEvents) && text.lengths == queries && text.reads == reads && destination.calls == calls);
            check(destination.size() == expected.size());
            for (int index = 0; index < expected.size(); index++) {
                Object value = destination.get(index); check(value instanceof Character && ((Character)value).charValue() == expected.get(index).charValue());
                if (expected.get(index).charValue() < 128) check(value == expected.get(index));
            }
            check(other.isEmpty() && late.lengths == 0 && late.reads == 0);
            check(Trace.text == (!expected.isEmpty() ? late : mode == 5 ? null : text) && Trace.destination == (!expected.isEmpty() ? other : mode == 6 || mode == 7 ? null : destination));
            check(Trace.after == (finished ? result : sentinel));
            if (failureKind == 5) check(attempt == 0 ? failure instanceof ExceptionInInitializerError && failure.getCause() == Trace.failure : failure instanceof NoClassDefFoundError);
            else if (failureKind == 1) check(failure == Trace.failure);
            else if (failureKind == 2) check(failure == Trace.error);
            else if (failureKind == 3) check(failure instanceof NullPointerException);
            else if (failureKind == 4) check(failure instanceof ArrayIndexOutOfBoundsException);
            else if (failureKind == 6) check(failure instanceof ClassCastException);
            else check(failure == null && actual == result);
            System.out.println("route=" + route + ",operation=" + operation + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.events);
        }
    }
}
