import fixture.*;
public final class ScopedCompletionNullReceiverHarness {
    public static void main(String[] args) {
        int[] calls={0};Object sentinel=new Object();
        Completion completion=new Completion() {
            public Object context() { calls[0]++;return sentinel; }
            public void accept(Object value) { calls[0]++; }
        };
        Client.receiver=null;Engine.context=sentinel;Engine.seenReceiver=new Engine();
        Provider previous=Engine.seenReceiver;
        for(int route=0;route<2;route++) {
            Throwable caught=null;
            try {
                if(route==0)Client.call(null,completion);
                else ScopedCompletionCallbackCaller.call(null,completion);
            } catch(Throwable failure) { caught=failure; }
            if(!(caught instanceof NullPointerException)||calls[0]!=0||Engine.context!=sentinel||Engine.seenReceiver!=previous)
                throw new AssertionError("owned scoped completion null receiver boundary");
        }
        System.out.println("null-receiver-before-provider-body=retained");
    }
}
