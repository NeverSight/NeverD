import fixture.Rule;

public class MappedConsumerFactoryFuture implements Rule<String> {
    public String first;
    public String second;
    public int calls;
    public int answer;
    public Throwable failure;

    @SuppressWarnings("unchecked")
    private static <T extends Throwable> void raise(Throwable failure) throws T {
        throw (T) failure;
    }

    public int compare(String first, String second) {
        ++calls;
        this.first = first;
        this.second = second;
        if (failure != null) MappedConsumerFactoryFuture.<RuntimeException>raise(failure);
        return answer;
    }
}
