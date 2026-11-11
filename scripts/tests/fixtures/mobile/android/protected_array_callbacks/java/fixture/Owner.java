package fixture;

public class Owner {
    public int index;
    public int callbacks;

    public Owner(int index) { this.index = index; }

    public Worker create() { return new Worker(); }

    public final class Worker extends Base<String, Integer, String> {
        public int calls;
        public int cancelCalls;
        public int finishCalls;
        public String[] lastArray;
        public String lastResult;
        public RuntimeException failure;

        public Worker() {}

        protected String run(String... values) {
            calls++;
            lastArray = values;
            trace = trace * 10 + 4;
            if (failure != null) throw failure;
            lastResult = values[index];
            return lastResult;
        }

        protected void cancel(String result) {
            cancelCalls++;
            callbacks++;
            lastResult = result;
            trace = trace * 10 + 5;
            if (failure != null) throw failure;
        }

        protected void finish(String result) {
            finishCalls++;
            callbacks++;
            lastResult = result;
            trace = trace * 10 + 6;
            if (failure != null) throw failure;
        }
    }
}
