package fixture;
public class Owner {
    public Class<?> witness;
    public Item[] items;
    public int calls, lastIndex;
    public RuntimeException failure;
    public class Worker extends Base {
        public Worker() {}
        public Item getItem(int index) {
            Owner.this.calls++; Owner.this.lastIndex=index;
            if (Owner.this.failure!=null) throw Owner.this.failure;
            return Owner.this.items[index];
        }
    }
    private class PrivateWorker extends Base {
        public PrivateWorker() {}
        public Item getItem(int index) {
            Owner.this.calls++; Owner.this.lastIndex=index;
            if (Owner.this.failure!=null) throw Owner.this.failure;
            return Owner.this.items[index];
        }
    }
    public Worker createPublic() { return new Worker(); }
    public Base createPrivate() { return new PrivateWorker(); }
}
