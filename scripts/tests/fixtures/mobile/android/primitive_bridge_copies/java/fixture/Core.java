package fixture;
public class Core<G extends CharSequence> extends Parent<G> {
    public int calls, seenNumber;
    public Object seenMarker;
    public CharSequence seenInput;
    public float seenSingle;
    public long seenInteger;
    public double seenDecimal;
    public RuntimeException failure;
    @Override
    int scalar(Object marker, G input, int number, float single, long integer, double decimal) {
        calls++;
        seenMarker = marker;
        seenInput = input;
        seenNumber = number;
        seenSingle = single;
        seenInteger = integer;
        seenDecimal = decimal;
        if (failure != null) throw failure;
        return number;
    }
    @Override
    float single(Object marker, G input, int number, float single, long integer, double decimal) {
        calls++;
        seenMarker = marker;
        seenInput = input;
        seenNumber = number;
        seenSingle = single;
        seenInteger = integer;
        seenDecimal = decimal;
        if (failure != null) throw failure;
        return single;
    }
    @Override
    long wide(Object marker, G input, int number, float single, long integer, double decimal) {
        calls++;
        seenMarker = marker;
        seenInput = input;
        seenNumber = number;
        seenSingle = single;
        seenInteger = integer;
        seenDecimal = decimal;
        if (failure != null) throw failure;
        return integer;
    }
    @Override
    double decimal(Object marker, G input, int number, float single, long integer, double decimal) {
        calls++;
        seenMarker = marker;
        seenInput = input;
        seenNumber = number;
        seenSingle = single;
        seenInteger = integer;
        seenDecimal = decimal;
        if (failure != null) throw failure;
        return decimal;
    }
}
