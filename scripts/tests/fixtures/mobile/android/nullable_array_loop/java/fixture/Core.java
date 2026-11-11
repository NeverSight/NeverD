package fixture;
public final class Core {
    public static String[] copy(String[] input) {
        String[] values = null;
        if (input != null) values = new String[input.length];
        Provider.fill(values);
        if (values != null)
            for (int i = 0; i < values.length; i++) input[i] = Provider.wrap(values[i]);
        return input;
    }
}
