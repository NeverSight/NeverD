package fixture;
public class Owner {
    public Object[] cells;
    public Cursor cursor(int offset) { return new Cursor(offset); }
    public class Cursor implements Api {
        public int offset;
        public Cursor(int offset) { this.offset = offset; Trace.hit(1); }
        @SuppressWarnings("unchecked")
        public <T> T get(int index) {
            Trace.hit(3);
            Object[] cells = Owner.this.cells;
            return (T) cells[offset + index];
        }
        public Owner owner() { return Owner.this; }
    }
}
