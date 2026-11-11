import fixture.Api;
import fixture.Owner;
import fixture.Trace;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.TypeVariable;

public final class CapturedScalarGetterHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("captured scalar getter changed");
    }
    static final class Derived extends Owner.Cursor {
        Object returned;
        Derived(Owner owner, int offset) { owner.super(offset); }
        @SuppressWarnings("unchecked")
        public <T> T get(int index) { Trace.hit(3); return (T) returned; }
    }
    static final class Marker {
        final String text;
        final RuntimeException failure;
        final boolean fail;
        Marker(String text, RuntimeException failure, boolean fail) {
            this.text = text; this.failure = failure; this.fail = fail;
        }
        public String toString() {
            Trace.hit(8);
            if (fail) throw failure;
            return text;
        }
    }
    static final class Hook implements Runnable {
        final Owner owner;
        final Owner.Cursor cursor;
        final Object value;
        final int mode;
        final RuntimeException failure;
        final Error error;
        int calls;
        Hook(Owner owner, Owner.Cursor cursor, Object value, int mode, RuntimeException failure, Error error) {
            this.owner = owner; this.cursor = cursor; this.value = value; this.mode = mode;
            this.failure = failure; this.error = error;
        }
        public void run() {
            calls++;
            if (mode == 9) throw failure;
            if (mode == 10) throw error;
            owner.cells = new Object[]{value, new Object(), new Object()}; cursor.offset = -1;
        }
    }
    static void contract() throws Exception {
        Class<?> cursor = Class.forName("fixture.Owner$Cursor", false, CapturedScalarGetterHarness.class.getClassLoader());
        require(cursor.getTypeParameters().length == 0 && Owner.class.getTypeParameters().length == 0);
        require(cursor.getDeclaringClass() == Owner.class && cursor.isMemberClass());
        require((cursor.getModifiers() & ~Modifier.STATIC) == Modifier.PUBLIC);
        require(cursor.getDeclaredConstructor(Owner.class, int.class).getModifiers() == Modifier.PUBLIC);
        Field capture = cursor.getDeclaredField("this$0");
        require(capture.getType() == Owner.class && (capture.getModifiers() & ~0x1000) == Modifier.FINAL);
        for (Class<?> declaration : new Class<?>[]{cursor, Api.class}) {
            Method method = declaration.getDeclaredMethod("get", int.class);
            TypeVariable<?>[] formals = method.getTypeParameters();
            require(formals.length == 1 && formals[0].getName().equals("T"));
            require(formals[0].getGenericDeclaration().equals(method) && formals[0].getBounds().length == 1
                && formals[0].getBounds()[0] == Object.class && method.getGenericReturnType().equals(formals[0]));
            require(method.getGenericParameterTypes().length == 1 && method.getGenericParameterTypes()[0] == int.class
                && method.getGenericExceptionTypes().length == 0 && !Modifier.isStatic(method.getModifiers()));
        }
    }
    public static void main(String[] args) throws Throwable {
        int route = Integer.parseInt(args[0]), kind = Integer.parseInt(args[1]), mode = Integer.parseInt(args[2]);
        require(route >= 0 && route < 6 && kind >= 0 && kind < 3 && mode >= 0 && mode < 12);
        contract();
        for (int attempt = 0; attempt < 2; attempt++) {
            Trace.hook = null;
            RuntimeException failure = new IllegalStateException("owned getter failure");
            Error error = new AssertionError("owned getter error");
            Owner owner = new Owner();
            Owner.Cursor cursor = kind == 1 ? new Derived(owner, 1)
                : attempt == 0 ? owner.new Cursor(1) : owner.cursor(1);
            require(cursor.owner() == owner);
            Object selected = mode == 1 ? new String(kind == 1 ? "override" : "base") : mode == 2 ? null
                : mode == 3 || mode == 11 ? new Marker(kind == 1 ? "override-marker" : "base-marker", failure, mode == 11)
                : mode == 4 ? new int[]{attempt} : new char[]{kind == 1 ? 'o' : 'b', 'x'};
            Object replacement = new long[]{attempt, mode};
            owner.cells = new Object[]{new Object(), new Object(), selected};
            if (kind == 1) ((Derived)cursor).returned = selected;
            if (mode == 5) owner.cells = null;
            if (mode == 7) cursor.offset = 91;
            int index = mode == 6 ? 91 : 1;
            Hook hook = new Hook(owner, cursor, replacement, mode, failure, error);
            Trace.hook = mode >= 8 && mode <= 10 ? hook : null;
            Object expected = mode == 8 && kind == 0 ? replacement : selected;
            String expectedAppend = expected == null ? "null" : expected instanceof Marker ? ((Marker)expected).text : expected.toString();
            Trace.trace = 0;
            Throwable caught = null; Object result = new Object();
            try { result = CapturedScalarGetterCaller.call(route, kind == 2 ? null : cursor, index); }
            catch (Throwable thrown) { caught = thrown; }
            boolean getterFailure = kind == 2 || mode == 9 || mode == 10
                || (kind == 0 && (mode == 5 || mode == 6 || mode == 7));
            boolean castFailure = !getterFailure && (route == 2 || route == 3)
                && expected != null && !(expected instanceof String);
            boolean textFailure = !getterFailure && route >= 4 && expected instanceof Marker && mode == 11;
            if (kind == 2) require(caught instanceof NullPointerException);
            else if (mode == 9 || textFailure) require(caught == failure);
            else if (mode == 10) require(caught == error);
            else if (getterFailure && mode == 5) require(caught instanceof NullPointerException);
            else if (getterFailure) require(caught instanceof ArrayIndexOutOfBoundsException);
            else if (castFailure) require(caught instanceof ClassCastException);
            else {
                require(caught == null);
                if (route < 4) require(result == expected);
                else require(expectedAppend.equals(result));
            }
            long trace = kind == 2 ? 0 : getterFailure ? 3 : route >= 4 && expected instanceof Marker ? 378 : 37;
            require(Trace.trace == trace && hook.calls == (kind != 2 && mode >= 8 && mode <= 10 ? 1 : 0));
            System.out.println("route=" + route + ",kind=" + kind + ",mode=" + mode + ",attempt=" + attempt + ",trace=" + Trace.trace);
        }
    }
}
