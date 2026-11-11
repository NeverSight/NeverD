package fixture;
public class Owner {
    public class Worker extends Base {
        public Worker(Object payload) {
            super(payload, Provider.word, null);
            Trace.hit(4);
        }
        public Owner owner() { return Owner.this; }
    }
    public class WideWorker extends Base {
        public WideWorker(Object payload) {
            super(payload, Provider.wide, null);
            Trace.hit(4);
        }
        public Owner owner() { return Owner.this; }
    }
    public Worker create(Object payload) { return new Worker(payload); }
    public WideWorker createWide(Object payload) { return new WideWorker(payload); }
}
