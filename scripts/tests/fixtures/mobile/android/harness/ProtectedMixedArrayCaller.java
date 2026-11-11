package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedMixedArrayCaller {
    public static void erased(Base self, Token token, Object array, int index) { self.dispatch(token, array, index); }
    public static void typed(Object self, Token token, Object array, int index, int kind) {
        switch (kind) {
        case 0: ((BooleanCore)self).write(token, (boolean[])array, index); return;
        case 1: ((ByteCore)self).write(token, (byte[])array, index); return;
        case 2: ((ShortCore)self).write(token, (short[])array, index); return;
        case 3: ((CharacterCore)self).write(token, (char[])array, index); return;
        case 4: ((IntegerCore)self).write(token, (int[])array, index); return;
        case 5: ((LongCore)self).write(token, (long[])array, index); return;
        case 6: ((FloatCore)self).write(token, (float[])array, index); return;
        case 7: ((DoubleCore)self).write(token, (double[])array, index); return;
        default: throw new AssertionError("kind");
        }
    }
}
