package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public abstract class Core<V> implements Task<V> {
    protected Core() {}
    public static Object call(Task task) throws Failure { return read(task); }
    public static Base typed(Task<Base> task) throws Failure { return read(task); }
    static <V> V read(Task<V> task) throws Failure {
        Trace.hit(1, task, null, 0);
        boolean retried = false;
        int attempts = 0;
        try {
            for (;;) {
                ++attempts;
                try {
                    V result = task.get();
                    Trace.hit(2, task, result, attempts);
                    return result;
                } catch (Retry retry) {
                    retried = true;
                    Trace.hit(3, task, retry, attempts);
                }
            }
        } finally {
            if (retried) Trace.hit(4, task, null, attempts);
            Trace.hit(5, task, null, attempts);
        }
    }
}
