package fixture;

public class SharedHost {
    public static Object make(int input) { return new Worker(input); }
    public static Object another(int input) { return new Worker(input); }
    public static int read(Object object) { return ((Worker) object).value; }
    public static int count() { return Worker.count; }
    public static Adapter wrap(Object object) {
        Worker worker = (Worker) object;
        Worker.Child child = (Worker.Child) worker.nested();
        return new Adapter(child);
    }

    public static final class Worker {
        static int count;
        public final int value;
        public Worker(int input) {
            value = input;
            count++;
        }
        Object nested() { return new Child(this); }
        static final class Child {
            final Worker parent;
            Child(Worker parent) { this.parent = parent; }
            int value() { return parent.value; }
        }
    }
}
