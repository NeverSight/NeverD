package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public final class ProtectedArrayGetterCaller {
    public static Object erased(Base self) { return self.dispatch(); }
    public static Object typed(Object self, int kind) {
        switch (kind) {
        case 0: return ((BooleanCore)self).get();
        case 1: return ((ByteCore)self).get();
        case 2: return ((ShortCore)self).get();
        case 3: return ((CharacterCore)self).get();
        case 4: return ((IntegerCore)self).get();
        case 5: return ((LongCore)self).get();
        case 6: return ((FloatCore)self).get();
        case 7: return ((DoubleCore)self).get();
        default: throw new AssertionError("kind");
        }
    }
}
