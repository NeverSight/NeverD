package fixture
object Owner {
    @JvmField val shared: Api = object : Api {
        override fun <R, S> echo(token: Class<R>?, input: R): R =
            Trace.echo<R>(this, token, input)
        override fun inspect(input: Carrier<Number, String>?): Carrier<Number, String>? =
            Trace.inspect(this, input)
    }
}
