package fixture;
public class Owner {
    public Object token;
    public class Worker {
        @SuppressWarnings("unchecked")
        private <T> T read(String label, Class<?>[] kinds, Object[] values, long wide, double real) {
            Trace.hit(1);
            Trace.receiver = this; Trace.owner = Owner.this; Trace.label = label;
            Trace.kinds = kinds; Trace.values = values; Trace.wide = wide; Trace.real = real;
            Trace.hit(2);
            return (T) Provider.pick(Owner.this.token, kinds, values);
        }
        public Object raw(Worker other, String label, Class<?>[] kinds, Object[] values, long wide, double real) {
            return other.<Object>read(label, kinds, values, wide, real);
        }
        public String narrow(Worker other, String label, Class<?>[] kinds, Object[] values, long wide, double real) {
            return other.<String>read(label, kinds, values, wide, real);
        }
    }
}
