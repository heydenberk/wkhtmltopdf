// -*- mode: c++; tab-width: 4; indent-tabs-mode: t; eval: (progn (c-set-style "stroustrup") (c-set-offset 'innamespace 0)); -*-
// vi:set ts=4 sts=4 sw=4 noet :
//
// Copyright 2010, 2011 wkhtmltopdf authors
//
// This file is part of wkhtmltopdf.
//
// wkhtmltopdf is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// wkhtmltopdf is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with wkhtmltopdf.  If not, see <http://www.gnu.org/licenses/>.

#ifdef __WKHTMLTOX_UNDEF_QT_DLL__
#ifdef QT_DLL
#undef QT_DLL
#endif
#endif

#include "multipageloader_p.hh"
#include <QFile>
#include <QFileInfo>
#include <QNetworkCookie>
#include <QNetworkDiskCache>
#include <QTimer>
#include <QUuid>
#include <iostream>

namespace wkhtmltopdf {
/*!
  \file multipageloader.hh
  \brief Defines the MultiPageLoader class
*/

/*!
  \file multipageloader_p.hh
  \brief Defines the MultiPageLoaderPrivate class
*/


LoaderObject::LoaderObject(QWebPage & p): page(p), skip(false) {};

MyNetworkAccessManager::MyNetworkAccessManager(const settings::LoadPage & s): 
	disposed(false),
	settings(s) {

	if ( !s.cacheDir.isEmpty() ){
		QNetworkDiskCache *cache = new QNetworkDiskCache(this);
		cache->setCacheDirectory(s.cacheDir);
		QNetworkAccessManager::setCache(cache);
	}
}

void MyNetworkAccessManager::dispose() {
	disposed = true;
}

void MyNetworkAccessManager::allow(QString path) {
	QString x = QFileInfo(path).canonicalFilePath();
	if (x.isEmpty()) return;
	allowed.insert(x);
}

QNetworkReply * MyNetworkAccessManager::createRequest(Operation op, const QNetworkRequest & req, QIODevice * outgoingData) {

	if (disposed)
	{
		emit warning("Received createRequest signal on a disposed ResourceObject's NetworkAccessManager. "
				 "This migth be an indication of an iframe taking to long to load.");
		// Needed to avoid race conditions by spurious network requests
		// by scripts or iframes taking too long to load.
		QNetworkRequest r2 = req;
		r2.setUrl(QUrl("about:blank"));
		return QNetworkAccessManager::createRequest(op, r2, outgoingData);
	}

	if (req.url().scheme() == "file" && settings.blockLocalFileAccess) {
		bool ok=false;
		QString path = QFileInfo(req.url().toLocalFile()).canonicalFilePath();
		QString old = "";
		while (path != old) {
			if (allowed.contains(path)) {
				ok=true;
				break;
			}
			old = path;
			path = QFileInfo(path).path();
		}
		if (!ok) {
			QNetworkRequest r2 = req;
			emit warning(QString("Blocked access to file %1").arg(QFileInfo(req.url().toLocalFile()).canonicalFilePath()));
			r2.setUrl(QUrl("about:blank"));
			return QNetworkAccessManager::createRequest(op, r2, outgoingData);
		}
	}
	QNetworkRequest r3 = req;
	if (settings.repeatCustomHeaders) {
		typedef QPair<QString, QString> HT;
		foreach (const HT & j, settings.customHeaders)
			r3.setRawHeader(j.first.toAscii(), j.second.toAscii());
	}
	return QNetworkAccessManager::createRequest(op, r3, outgoingData);
}


MyQWebPage::MyQWebPage(ResourceObject & res): resource(res) {}

void MyQWebPage::javaScriptAlert(QWebFrame *, const QString & msg) {
	resource.warning(QString("Javascript alert: %1").arg(msg));
}

bool MyQWebPage::javaScriptConfirm(QWebFrame *, const QString & msg) {
	resource.warning(QString("Javascript confirm: %1 (answered yes)").arg(msg));
	return true;
}

bool MyQWebPage::javaScriptPrompt(QWebFrame *, const QString & msg, const QString & defaultValue, QString * result) {
	resource.warning(QString("Javascript prompt: %1 (answered %2)").arg(msg,defaultValue));
	result = (QString*)&defaultValue;
	return true;
}

void MyQWebPage::javaScriptConsoleMessage(const QString & message, int lineNumber, const QString & sourceID) {
	if (resource.settings.debugJavascript)
		resource.warning(QString("%1:%2 %3").arg(sourceID).arg(lineNumber).arg(message));
}

bool MyQWebPage::shouldInterruptJavaScript() {
	if (resource.settings.stopSlowScripts) {
		resource.warning("A slow script was stopped");
		return true;
	}
	return false;
}

ResourceObject::ResourceObject(MultiPageLoaderPrivate & mpl, const QUrl & u, const settings::LoadPage & s):
	networkAccessManager(s),
	url(u),
	loginTry(0),
	progress(0),
	finished(false),
	signalPrint(false),
	multiPageLoader(mpl),
	webPage(*this),
	lo(webPage),
	httpErrorCode(0),
	settings(s) {

	connect(&networkAccessManager, SIGNAL(authenticationRequired(QNetworkReply*, QAuthenticator *)),this,
			SLOT(handleAuthenticationRequired(QNetworkReply *, QAuthenticator *)));
	foreach (const QString & path, s.allowed)
		networkAccessManager.allow(path);
	if (url.scheme() == "file")
		networkAccessManager.allow(url.toLocalFile());

	connect(&webPage, SIGNAL(loadStarted()), this, SLOT(loadStarted()));
	connect(&webPage, SIGNAL(loadProgress(int)), this, SLOT(loadProgress(int)));
	connect(&webPage, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));
	connect(&webPage, SIGNAL(printRequested(QWebFrame*)), this, SLOT(printRequested(QWebFrame*)));

	//If some ssl error occurs we want sslErrors to be called, so the we can ignore it
	connect(&networkAccessManager, SIGNAL(sslErrors(QNetworkReply*, const QList<QSslError>&)),this,
			SLOT(sslErrors(QNetworkReply*, const QList<QSslError>&)));

	connect(&networkAccessManager, SIGNAL(finished (QNetworkReply *)),
			this, SLOT(amfinished (QNetworkReply *) ) );

	connect(&networkAccessManager, SIGNAL(warning(const QString &)),
			this, SLOT(warning(const QString &)));

	networkAccessManager.setCookieJar(multiPageLoader.cookieJar);

	//If we must use a proxy, create a host of objects
	if (!settings.proxy.host.isEmpty()) {
		QNetworkProxy proxy;
		proxy.setHostName(settings.proxy.host);
		proxy.setPort(settings.proxy.port);
		proxy.setType(settings.proxy.type);
		// to retrieve a web page, it's not needed to use a fully transparent
		// http proxy. Moreover, the CONNECT() method is frequently disabled
		// by proxies administrators.
#if QT_VERSION >= 0x040500
		if (settings.proxy.type == QNetworkProxy::HttpProxy)
			proxy.setCapabilities(QNetworkProxy::CachingCapability |
								  QNetworkProxy::TunnelingCapability);
#endif
		if (!settings.proxy.user.isEmpty())
			proxy.setUser(settings.proxy.user);
		if (!settings.proxy.password.isEmpty())
			proxy.setPassword(settings.proxy.password);
		networkAccessManager.setProxy(proxy);
	}

	webPage.setNetworkAccessManager(&networkAccessManager);
	webPage.mainFrame()->setZoomFactor(settings.zoomFactor);
}

/*!
 * Once loading starting, this is called
 */
void ResourceObject::loadStarted() {
	if (finished == true) {
		++multiPageLoader.loading;
		finished = false;
	}
	if (multiPageLoader.loadStartedEmitted) return;
	multiPageLoader.loadStartedEmitted=true;
	emit multiPageLoader.outer.loadStarted();
}


/*!
 * Called when the page is loading, display some progress to the using
 * \param progress the loading progress in percent
 */
void ResourceObject::loadProgress(int p) {
	// If we are finished, ignore this signal.
	if (finished || multiPageLoader.resources.size() <= 0) {
		warning("A finished ResourceObject received a loading progress signal. "
			"This migth be an indication of an iframe taking to long to load.");
		return;
	}

	multiPageLoader.progressSum -= progress;
	progress = p;
	multiPageLoader.progressSum += progress;

	emit multiPageLoader.outer.loadProgress(multiPageLoader.progressSum / multiPageLoader.resources.size());
}

QString ResourceObject::evaluateJavaScript(const QString & str) {
	return webPage.mainFrame()->evaluateJavaScript(str).toString();
}

QStringList ResourceObject::evaluateJavaScripts(const QStringList & strs) {
	QStringList results;
	for (QStringList::const_iterator it=strs.begin(); it != strs.end(); ++it) {
		results.append(evaluateJavaScript(*it));
	}
	return results;
}

void ResourceObject::loadFinished(bool ok) {
	// If we are finished, this migth be a potential bug.
	if (finished || multiPageLoader.resources.size() <= 0) {
		warning("A finished ResourceObject received a loading finished signal. "
			"This migth be an indication of an iframe taking to long to load.");
		return;
	}

	multiPageLoader.hasError = multiPageLoader.hasError || (!ok && settings.loadErrorHandling == settings::LoadPage::abort);
	if (!ok) {
		if (settings.loadErrorHandling == settings::LoadPage::abort)
			error(QString("Failed loading page ") + url.toString() + " (sometimes it will work just to ignore this error with --load-error-handling ignore)");
		else if (settings.loadErrorHandling == settings::LoadPage::skip) {
			warning(QString("Failed loading page ") + url.toString() + " (skipped)");
			lo.skip = true;
		} else
			warning(QString("Failed loading page ") + url.toString() + " (ignored)");
	}

	const char *text = 
		"(function() {\n"
		"    var layerNodes = document.querySelectorAll(\".layer\");\n"
		"    var clickzones = Array.prototype.map.call(layerNodes, function(layerNode) {\n"
		"        var maxWidth = window.innerWidth, maxHeight = window.innerHeight;\n"
		"        var rect = layerNode.getBoundingClientRect();\n"
		"        var left = parseInt(layerNode.style.left.slice(0, -2), 10);\n"
		"        var top = parseInt(layerNode.style.top.slice(0, -2), 10);\n"
		"        if (left > maxWidth || top > maxHeight) return null;\n"
		"\n"
		"        var clickzoneProperties = {\n"
		"            \"alt\": \"alt\",\n"
		"            \"css_class\": \"cls\",\n"
		"            \"href\": \"href\",\n"
		"            \"layerId\": \"layerId\",\n"
		"            \"popup_height\": \"popHeight\",\n"
		"            \"popup_menubar\": \"popMenubar\",\n"
		"            \"popup_name\": \"pop\",\n"
		"            \"popup_resize\": \"popResizable\",\n"
		"            \"popup_scrollbar\": \"popScrollbars\",\n"
		"            \"popup_statusbar\": \"popStatus\",\n"
		"            \"popup_toolbar\": \"popToolbar\",\n"
		"            \"popup_width\": \"popWidth\",\n"
		"            \"rel\": \"r\",\n"
		"            \"target\": \"t\"\n"
		"        };\n"
		"        var values = {\n"
		"            x: left,\n"
		"            y: top,\n"
		"            w: Math.min(rect.width, maxWidth - left),\n"
		"            h: Math.min(rect.height, maxHeight - top)\n"
		"        };\n"
		"\n"
		"        // Need to set appropriate type to allow for proper serialization of BackendActionInputs\n"
		"        // since we are not performing this on the front end by loading up each layer with empty dataset values\n"
		"        var boolProperties = [\"popup_menubar\", \"popup_resize\", \"popup_scrollbar\", \"popup_statusbar\", \"popup_toolbar\"];\n"
		"\n"
		"        Object.keys(clickzoneProperties).forEach(function(property) {\n"
		"            var missingValueDefault =  boolProperties.indexOf(property) >= 0 ? \"no\" : \"\";\n"
		"            var camelKey = clickzoneProperties[property];\n"
		"            var propertyValue = layerNode.dataset[camelKey] || layerNode.dataset[property] || missingValueDefault;\n"
		"            if( boolProperties.indexOf(property) >= 0 ) { // look in frontend display.js\n"
		"                propertyValue = (propertyValue && propertyValue.toUpperCase() === \"TRUE\") ? \"yes\" : \"no\"\n"
		"            }\n"
		"            values[camelKey] = propertyValue;\n"
		"        });\n"
		"\n"
		"        if (!values.href) return null;\n"
		"\n"
		"        // Also map alt to title on the frontend.\n"
		"        values.l = values.alt;\n"
		"\n"
		"        return values;\n"
		"    });\n"
		"\n"
		"    return clickzones;\n"
		"})();";
	std::cout << text;
	QStringList sl;
	sl.append(QString(text));
	settings.runScript = sl;
	// Evaluate extra user supplied javascript
	QStringList r = evaluateJavaScripts(settings.runScript);
	std::cout << "   runResults: " << r.join(":").toLocal8Bit().constData() << std::endl;

	// XXX: If loading failed there's no need to wait
	//      for javascript on this resource.
	if (!ok || signalPrint || settings.jsdelay == 0) loadDone();
	else if (!settings.windowStatus.isEmpty()) waitWindowStatus();
	else QTimer::singleShot(settings.jsdelay, this, SLOT(loadDone()));

}

void ResourceObject::waitWindowStatus() {
	QString windowStatus = webPage.mainFrame()->evaluateJavaScript("window.status").toString();
	//warning(QString("window.status:" + windowStatus + " settings.windowStatus:" + settings.windowStatus));
	if (windowStatus != settings.windowStatus) {
		QTimer::singleShot(50, this, SLOT(waitWindowStatus()));
	} else {
		QTimer::singleShot(settings.jsdelay, this, SLOT(loadDone()));
	}
}

void ResourceObject::printRequested(QWebFrame *) {
	signalPrint=true;
	loadDone();
}

void ResourceObject::loadDone() {
	if (finished) return;
	finished=true;

	// Ensure no more loading goes..
	webPage.triggerAction(QWebPage::Stop);
	webPage.triggerAction(QWebPage::StopScheduledPageRefresh);
	networkAccessManager.dispose();
	//disconnect(this, 0, 0, 0);

	--multiPageLoader.loading;
	if (multiPageLoader.loading == 0)
		multiPageLoader.loadDone();
}

/*!
 * Called when the page requires authentication, fills in the username
 * and password supplied on the command line
 */
void ResourceObject::handleAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator) {

	// XXX: Avoid calling 'reply->abort()' from within this signal.
	//      As stated by doc, request would be finished when no
	//      user/pass properties are assigned to authenticator object.
	// See: http://qt-project.org/doc/qt-5.0/qtnetwork/qnetworkaccessmanager.html#authenticationRequired

	if (settings.username.isEmpty()) {
		//If no username is given, complain the such is required
		error("Authentication Required");
	} else if (loginTry >= 2) {
		//If the login has failed a sufficient number of times,
		//the username or password must be wrong
		error("Invalid username or password");
	} else {
		authenticator->setUser(settings.username);
		authenticator->setPassword(settings.password);
		++loginTry;
	}
}

void ResourceObject::warning(const QString & str) {
	emit multiPageLoader.outer.warning(str);
}

void ResourceObject::error(const QString & str) {
	emit multiPageLoader.outer.error(str);
}

/*!
 * Track and handle network errors
 * \param reply The networkreply that has finished
 */
void ResourceObject::amfinished(QNetworkReply * reply) {
	int networkStatus = reply->error();
	int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if ((networkStatus != 0 && networkStatus != 5) || (httpStatus > 399 && httpErrorCode == 0))
	{
		QFileInfo fi(reply->url().toString());
		bool mediaFile = settings::LoadPage::mediaFilesExtensions.contains(fi.completeSuffix().toLower());
		if ( ! mediaFile) {
			// XXX: Notify network errors as higher priority than HTTP errors.
			//      QT's QNetworkReply::NetworkError enum uses values overlapping
			//      HTTP status codes, so adding 1000 to QT's codes will avoid
			//      confusion. Also a network error at this point will probably mean
			//      no HTTP access at all, so we want network errors to be reported
			//      with a higher priority than HTTP ones.
			//      See: http://doc-snapshot.qt-project.org/4.8/qnetworkreply.html#NetworkError-enum
			httpErrorCode = networkStatus > 0 ? (networkStatus + 1000) : httpStatus;
			return;
		}
		if (settings.mediaLoadErrorHandling == settings::LoadPage::abort)
		{
			httpErrorCode = networkStatus > 0 ? (networkStatus + 1000) : httpStatus;
			error(QString("Failed to load ") + reply->url().toString() + ", with code: " + QString::number(httpErrorCode) + 
				" (sometimes it will work just to ignore this error with --load-media-error-handling ignore)");
		}
		else {
			warning(QString("Failed to load %1 (%2)")
					.arg(reply->url().toString())
					.arg(settings::loadErrorHandlingToStr(settings.mediaLoadErrorHandling))
					);
		}
	}
}

/*!
 * Handle any ssl error by ignoring
 */
void ResourceObject::sslErrors(QNetworkReply *reply, const QList<QSslError> &) {
	//We ignore any ssl error, as it is next to impossible to send or receive
	//any private information with wkhtmltopdf anyhow, seeing as you cannot authenticate
	reply->ignoreSslErrors();
	warning("SSL error ignored");
}

void ResourceObject::load() {
	finished=false;
	++multiPageLoader.loading;

	bool hasFiles=false;
	foreach (const settings::PostItem & pi, settings.post) hasFiles |= pi.file;
	QByteArray postData;
	QString boundary;
	if (hasFiles) {
		boundary = QUuid::createUuid().toString().remove('-').remove('{').remove('}');
		foreach (const settings::PostItem & pi, settings.post) {
			//TODO escape values here
			postData.append("--");
			postData.append(boundary);
			postData.append("\ncontent-disposition: form-data; name=\"");
			postData.append(pi.name);
			postData.append('\"');
			if (pi.file) {
				QFile f(pi.value);
				if (!f.open(QIODevice::ReadOnly) ) {
					error(QString("Unable to open file ")+pi.value);
					multiPageLoader.fail();
				}
				postData.append("; filename=\"");
				postData.append( QFileInfo(pi.value).fileName());
				postData.append("\"\n\n");
				postData.append( f.readAll() );
				//TODO ADD MIME TYPE
			} else {
				postData.append("\n\n");
				postData.append(pi.value);
			}
			postData.append('\n');
		}
		if (!postData.isEmpty()) {
			postData.append("--");
			postData.append(boundary);
			postData.append("--\n");
		}
	} else {
		QUrl u;
		foreach (const settings::PostItem & pi, settings.post)
			u.addQueryItem(pi.name, pi.value);
		postData = u.encodedQuery();
	}


	typedef QPair<QString, QString> SSP;
	foreach (const SSP & pair, settings.cookies)
		multiPageLoader.cookieJar->useCookie(url, pair.first, pair.second);

	QNetworkRequest r = QNetworkRequest(url);
	typedef QPair<QString, QString> HT;
	foreach (const HT & j, settings.customHeaders)
		r.setRawHeader(j.first.toAscii(), j.second.toAscii());

	if (postData.isEmpty())
		webPage.mainFrame()->load(r);
	else {
		if (hasFiles)
			r.setHeader(QNetworkRequest::ContentTypeHeader, QString("multipart/form-data, boundary=")+boundary);
		webPage.mainFrame()->load(r, QNetworkAccessManager::PostOperation, postData);
	}
}

void MyCookieJar::useCookie(const QUrl &, const QString & name, const QString & value) {
	extraCookies.push_back(QNetworkCookie(name.toUtf8(), value.toUtf8()));
}

QList<QNetworkCookie> MyCookieJar::cookiesForUrl(const QUrl & url) const {
	QList<QNetworkCookie> list = QNetworkCookieJar::cookiesForUrl(url);
	list.append(extraCookies);
	return list;
}

void MyCookieJar::loadFromFile(const QString & path) {
	QFile cookieJar(path);
	if (cookieJar.open(QIODevice::ReadOnly | QIODevice::Text) )
		setAllCookies(QNetworkCookie::parseCookies(cookieJar.readAll()));
}

void MyCookieJar::saveToFile(const QString & path) {
	QFile cookieJar(path);
	if (cookieJar.open(QIODevice::WriteOnly | QIODevice::Text) )
		foreach (const QNetworkCookie & cookie, allCookies()) {
			cookieJar.write(cookie.toRawForm());
			cookieJar.write(";\n");
		}
}

void MultiPageLoaderPrivate::loadDone() {
	 if (!settings.cookieJar.isEmpty())
		cookieJar->saveToFile(settings.cookieJar);

	if (!finishedEmitted) {
		finishedEmitted = true;
		emit outer.loadFinished(!hasError);
	}
}



/*!
 * Copy a file from some place to another
 * \param src The source to copy from
 * \param dst The destination to copy to
 */
bool MultiPageLoader::copyFile(QFile & src, QFile & dst) {
//      TODO enable again when
//      http://bugreports.qt.nokia.com/browse/QTBUG-6894
//      is fixed
//      QByteArray buf(1024*1024*5,0);
//      while ( qint64 r=src.read(buf.data(),buf.size())) {
//          if (r == -1) return false;
//          if (dst.write(buf.data(),r) != r) return false;
//      }

	if (dst.write( src.readAll() ) == -1) return false;

	src.close();
	dst.close();
	return true;
}

MultiPageLoaderPrivate::MultiPageLoaderPrivate(const settings::LoadGlobal & s, MultiPageLoader & o):
	outer(o), settings(s) {

	cookieJar = new MyCookieJar();

	if (!settings.cookieJar.isEmpty())
		cookieJar->loadFromFile(settings.cookieJar);
}

MultiPageLoaderPrivate::~MultiPageLoaderPrivate() {
	clearResources();
}

LoaderObject * MultiPageLoaderPrivate::addResource(const QUrl & url, const settings::LoadPage & page) {
	ResourceObject * ro = new ResourceObject(*this, url, page);
	resources.push_back(ro);

	return &ro->lo;
}

void MultiPageLoaderPrivate::load() {
	progressSum=0;
	loadStartedEmitted=false;
	finishedEmitted=false;
	hasError=false;
	loading=0;

	for (int i=0; i < resources.size(); ++i)
		resources[i]->load();

	if (resources.size() == 0) loadDone();
}

void MultiPageLoaderPrivate::clearResources() {
	while (resources.size() > 0)
	{
		// XXX: Using deleteLater() to dispose
		// resources, to avoid race conditions with
		// pending signals reaching a deleted resource.
		// Also, and we must avoid calling clear()
		// on resources list, is it tries to delete
		// each objet on removal.
		ResourceObject *tmp = resources.takeFirst();
		tmp->deleteLater();
	}
	tempIn.remove();
}

void MultiPageLoaderPrivate::cancel() {
	//foreach (QWebPage * page, pages)
	//	page->triggerAction(QWebPage::Stop);
}

void MultiPageLoaderPrivate::fail() {
	hasError = true;
	cancel();
	clearResources();
}

/*!
  \brief Construct a multipage loader object, load settings read from the supplied settings
  \param s The settings to be used while loading pages
*/
MultiPageLoader::MultiPageLoader(settings::LoadGlobal & s):
	d(new MultiPageLoaderPrivate(s, *this)) {
}

MultiPageLoader::~MultiPageLoader() {
	MultiPageLoaderPrivate *tmp = d;
	d = 0;
	tmp->deleteLater();
}

/*!
  \brief Add a resource, to be loaded described by a string
  @param string Url describing the resource to load
*/
LoaderObject * MultiPageLoader::addResource(const QString & string, const settings::LoadPage & s, const QString * data) {
	QString url=string;
	if (data && !data->isEmpty()) {
		url = d->tempIn.create(".html");
		QFile tmp(url);
		if (!tmp.open(QIODevice::WriteOnly) || tmp.write(data->toUtf8())==0) {
			emit error("Unable to create temporary file");
			return NULL;
		}
	} else if (url == "-") {
		QFile in;
		in.open(stdin,QIODevice::ReadOnly);
		url = d->tempIn.create(".html");
		QFile tmp(url);
		if (!tmp.open(QIODevice::WriteOnly) || !copyFile(in, tmp)) {
			emit error("Unable to create temporary file");
			return NULL;
		}
	}
	return addResource(guessUrlFromString(url), s);
}

/*!
  \brief Add a page to be loaded
  @param url Url of the page to load
*/
LoaderObject * MultiPageLoader::addResource(const QUrl & url, const settings::LoadPage & s) {
	return d->addResource(url, s);
}

/*!
  \brief Guess a url, by looking at a string

  (shamelessly copied from Arora Project)
  \param string The string the is suppose to be some kind of url
*/
QUrl MultiPageLoader::guessUrlFromString(const QString &string) {
	QString urlStr = string.trimmed();

	// check if the string is just a host with a port
	QRegExp hostWithPort(QLatin1String("^[a-zA-Z\\.]+\\:[0-9]*$"));
	if (hostWithPort.exactMatch(urlStr))
		urlStr = QLatin1String("http://") + urlStr;

	// Check if it looks like a qualified URL. Try parsing it and see.
	QRegExp test(QLatin1String("^[a-zA-Z]+\\://.*"));
	bool hasSchema = test.exactMatch(urlStr);
	if (hasSchema) {
		bool isAscii = true;
		foreach (const QChar &c, urlStr) {
			if (c >= 0x80) {
				isAscii = false;
				break;
			}
		}

		QUrl url;
		if (isAscii) {
			url = QUrl::fromEncoded(urlStr.toAscii(), QUrl::TolerantMode);
		} else {
			url = QUrl(urlStr, QUrl::TolerantMode);
		}
		if (url.isValid())
			return url;
	}

	// Might be a file.
	if (QFile::exists(urlStr)) {
		QFileInfo info(urlStr);
		return QUrl::fromLocalFile(info.absoluteFilePath());
	}

	// Might be a shorturl - try to detect the schema.
	if (!hasSchema) {
		int dotIndex = urlStr.indexOf(QLatin1Char('.'));
		if (dotIndex != -1) {
			QString prefix = urlStr.left(dotIndex).toLower();
			QString schema = (prefix == QLatin1String("ftp")) ? prefix : QLatin1String("http");
			QUrl url(schema + QLatin1String("://") + urlStr, QUrl::TolerantMode);
			if (url.isValid())
				return url;
		}
	}

	// Fall back to QUrl's own tolerant parser.
	QUrl url = QUrl(string, QUrl::TolerantMode);

	// finally for cases where the user just types in a hostname add http
	if (url.scheme().isEmpty())
		url = QUrl(QLatin1String("http://") + string, QUrl::TolerantMode);
	return url;
}

/*!
  \brief Return the most severe http error code returned during loading
 */
int MultiPageLoader::httpErrorCode() {
	int res=0;
	foreach (const ResourceObject * ro, d->resources)
		if (ro->httpErrorCode > res) res = ro->httpErrorCode;
	return res;
}

/*!
  \brief Begin loading all the resources added
*/
void MultiPageLoader::load() {
	d->load();
}

/*!
  \brief Clear all the resources
*/
void MultiPageLoader::clearResources() {
	d->clearResources();
}

/*!
  \brief Cancel the loading of the pages
*/
void MultiPageLoader::cancel() {
	d->cancel();
}

/*!
  \fn MultiPageLoader::loadFinished(bool ok)
  \brief Signal emitted when all pages have been loaded
  \param ok True if all the pages have been loaded sucessfully
*/

/*!
  \fn MultiPageLoader::loadProgress(int progress)
  \brief Signal emitted once load has progressed
  \param progress Progress in percent
*/

/*!
  \fn MultiPageLoader::loadStarted()
  \brief Signal emitted when loading has started
*/

/*!
  \fn void MultiPageLoader::warning(QString text)
  \brief Signal emitted when a none fatal warning has occured
  \param text A string describing the warning
*/

/*!
  \fn void MultiPageLoader::error(QString text)
  \brief Signal emitted when a fatal error has occured
  \param text A string describing the error
*/
}
