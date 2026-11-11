import fixture.*;
public final class PrivateResultDerived extends Owner.Worker {
    public PrivateResultDerived(Owner owner) { owner.super(); }
    public <T> T read(String label, Class<?>[] kinds, Object[] values, long wide, double real) {
        Trace.hit(9); return null;
    }
}
