package fixture;
public final class Codec implements Api<CharSequence, byte[]> {
    public final byte[] result;
    public final LeafFailure failure;
    public final FatalFailure fatal;
    public int calls;
    public Codec(byte[] result, LeafFailure failure, FatalFailure fatal) {
        this.result = result;
        this.failure = failure;
        this.fatal = fatal;
    }
    @Override
    public byte[] convert(CharSequence input) throws LeafFailure, FatalFailure {
        calls++;
        if (input == null) throw failure;
        if (input.length() == 0) throw fatal;
        return result;
    }
}
